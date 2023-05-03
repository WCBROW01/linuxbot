#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#include <sys/wait.h>

#include <concord/discord.h>
#include <concord/log.h>

#include "job.h"

enum job_status {
	JOB_WAITING,
	JOB_RUNNING,
	JOB_EXITED,
	JOB_TERMINATED,
	JOB_ERROR
};

static const char *status_string[] = {
	"is currently in the queue",
	"is running",
	"has exited successfully",
	"was terminated",
	"had an error"
};

struct job {
	job_uid_t uid;
	char *cmd;
	u64snowflake user_id;
	u64snowflake channel_id;
	atomic_int status;
};

struct job_thread_state {
	pthread_t thread;
	struct discord *client;
};

// number of job threads
#define N_JOB_THREADS 8

#define JOB_QUEUE_SIZE 16384

#define JOB_BUF_SIZE 131072

static struct job job_table[JOB_QUEUE_SIZE];
static size_t job_table_len = 0;
static pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct job *job_queue[JOB_QUEUE_SIZE];
static atomic_size_t queue_head = 0;
static atomic_size_t queue_tail = 0;
static atomic_size_t queue_len = 0;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

static struct job_thread_state thread_pool[N_JOB_THREADS];

// -1 if queue is full
static int job_queue_push(struct job *job) {
	int result = 0;
	pthread_mutex_lock(&queue_mutex);
	if (queue_len == JOB_QUEUE_SIZE) result = -1;
	else {
		job_queue[queue_tail++] = job;
		queue_tail &= JOB_QUEUE_SIZE - 1;
		++queue_len;
	}
	pthread_mutex_unlock(&queue_mutex);
	if (!result) pthread_cond_signal(&queue_cond);

	return result;
}

static void job_finish(struct job *job) {
	pthread_mutex_lock(&table_mutex);
	job->uid = -1;
	free(job->cmd);
	pthread_mutex_unlock(&table_mutex);
}

static pid_t run_command(const char *cmd, FILE **fp) {
	int shpipe[2];
	if (pipe(shpipe) == -1) {
		log_error("Failed to open pipe.");
		return -1;
	}

	pid_t shpid = fork();
	if (shpid == -1) {
		log_error("Failed to fork process for sh");
		return -1;
	} else if (shpid) {
		close(shpipe[1]);
		*fp = fdopen(shpipe[0], "r");
		return shpid;
	} else {
		dup2(shpipe[1], STDOUT_FILENO);
		dup2(shpipe[1], STDERR_FILENO);
		close(shpipe[0]);
		execl("/bin/sh", "sh", "-c", cmd, NULL);
	}

	return -1; // unreachable
}

static void execute_job(struct discord *client, struct job *job) {
	char msg[DISCORD_MAX_MESSAGE_LEN];
	char path[DISCORD_MAX_MESSAGE_LEN];
	snprintf(path, sizeof(path), "attachment://%s.ansi", job->cmd);

	FILE *fp;
	pid_t pid = run_command(job->cmd, &fp);
	if (pid == -1) {
		job->status = JOB_ERROR;
		snprintf(
			msg, sizeof(msg),
			"Hey <@%" PRIu64 ">! Your job %s.\n"
			"Error: %s",
			job->user_id, status_string[job->status], strerror(errno)
		);

		job_finish(job);

		struct discord_create_message res = {
			.content = msg,
			.allowed_mentions = &(struct discord_allowed_mention) {
				.users = &(struct snowflakes) {
					.size = 1,
					.array = &job->user_id
				}
			}
		};
		discord_create_message(client, job->channel_id, &res, NULL);
		return;
	}

	char output[JOB_BUF_SIZE];

	{
		int status;
		size_t bytes_written = 0;
		char buf[65536]; // temporary buffer
		job->status = JOB_RUNNING;
		do {
			size_t bytes_read = fread(buf, 1, sizeof(buf), fp);
			if (bytes_read > JOB_BUF_SIZE - 1 - bytes_written) {
				// make room in the buffer
				size_t old_text_len = JOB_BUF_SIZE - 1 - bytes_read;
				memmove(output, output + bytes_read, old_text_len);
				memcpy(output + old_text_len, buf, bytes_read);
				bytes_written = JOB_BUF_SIZE - 1;
			} else {
				memcpy(output + bytes_written, buf, bytes_read);
				bytes_written += bytes_read;
			}
		} while (!waitpid(pid, &status, WNOHANG));
		fclose(fp);

		job->status = WIFEXITED(status) ? JOB_EXITED : JOB_TERMINATED;
		output[bytes_written] = '\0';
	}

	struct discord_attachment attachment = {
		.content = output,
		.filename = path,
		.content_type = "text"
	};

	snprintf(
		msg, sizeof(msg),
		"Hey <@%" PRIu64 ">! Your job %s.\n"
		"Results of `%s`",
		job->user_id, status_string[job->status], job->cmd
	);

	job_finish(job);

	struct discord_create_message res = {
		.content = msg,
		.allowed_mentions = &(struct discord_allowed_mention) {
			.users = &(struct snowflakes) {
				.size = 1,
				.array = &job->user_id
			}
		},
		.attachments = &(struct discord_attachments) {
			.size = 1,
			.array = &attachment
		}
	};
	discord_create_message(client, job->channel_id, &res, NULL);
}

static void *job_thread_worker(void *arg) {
	struct job_thread_state *state = arg;
	while (1) {
		struct job *current_job;

		pthread_mutex_lock(&queue_mutex);
		while (queue_len == 0) pthread_cond_wait(&queue_cond, &queue_mutex);

		current_job = job_queue[queue_head++];
		queue_head &= JOB_QUEUE_SIZE - 1;
		--queue_len;

		pthread_mutex_unlock(&queue_mutex);

		execute_job(state->client, current_job);
	}

	return NULL;
}

void init_job_queue(struct discord *client) {
	memset(job_table, -1, sizeof(job_table));
	for (int i = 0; i < N_JOB_THREADS; ++i) {
		thread_pool[i].client = discord_clone(client);
		pthread_create(&thread_pool[i].thread, NULL, &job_thread_worker, &thread_pool[i]);
	}
}

// -1 if there is no room in the table
job_uid_t submit_job(char *cmd, u64snowflake user_id, u64snowflake channel_id) {
	job_uid_t result = -1;
	size_t table_index;
	pthread_mutex_lock(&table_mutex);
	if (job_table_len < JOB_QUEUE_SIZE) {
		do {
			result = random();
			table_index = result & (JOB_QUEUE_SIZE - 1);
		} while (job_table[table_index].uid != -1);

		job_table[table_index] = (struct job) {
			.uid = result,
			.cmd = strdup(cmd),
			.user_id = user_id,
			.channel_id = channel_id,
			.status = JOB_WAITING
		};
	}
	pthread_mutex_unlock(&table_mutex);

	if (result != -1) job_queue_push(&job_table[table_index]);

	return result;
}
