#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <sys/wait.h>

#include <concord/discord.h>
#include <concord/log.h>

pid_t run_command(const char *cmd, FILE **fp) {
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
		close(shpipe[0]);
		execl("/bin/sh", "sh", "-c", cmd, NULL);
	}
}

void on_ready(struct discord *client, const struct discord_ready *event) {
	log_info("Logged in as %s!", event->user->username);

	// create commands
	struct discord_application_command_option options[] = {
		{
			.type = DISCORD_APPLICATION_OPTION_STRING,
			.name  = "command",
			.description = "Your command",
			.required = true
		}
	};
	struct discord_create_global_application_command sh_cmd = {
		.name = "sh",
		.description = "Run a shell command.",
		.default_permission = true,
		.dm_permission = true,
		.options = &(struct discord_application_command_options) {
			.size = sizeof(options) / sizeof(*options),
			.array = options
		}
	};
	discord_create_global_application_command(client, event->application->id, &sh_cmd, NULL);
}

void on_interaction(struct discord *client, const struct discord_interaction *event) {
	if (event->type != DISCORD_INTERACTION_APPLICATION_COMMAND)
		return; // not a slash command

	if (!strcmp(event->data->name, "sh")) {
		char *cmd = event->data->options->array[0].value;
		//struct discord_embed embed = { .title = cmd };
		char msg[DISCORD_MAX_MESSAGE_LEN];
		snprintf(msg, sizeof(msg), "Results of `%s`", cmd);
		char path[DISCORD_MAX_MESSAGE_LEN];
		snprintf(path, sizeof(path), "attachment://%s.ansi", cmd);

		FILE *fp;
		pid_t pid = run_command(cmd, &fp);
		if (pid == -1) {
			snprintf(msg, sizeof(msg), "Failed to run `%s`: %s", cmd, strerror(errno));
			struct discord_interaction_response res = {
				.type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
				.data = &(struct discord_interaction_callback_data) {
					.content = msg
				}
			};

			discord_create_interaction_response(client, event->id, event->token, &res, NULL);
			return;
		}

		int status;
		size_t bufsize = 65536;
		size_t bytes_written = 0;
		char *buf = malloc(bufsize);
		do {
			waitpid(pid, &status, WNOHANG);
			if (bufsize == bytes_written - 1) {
				char *new_buf = realloc(buf, bufsize * 2);
				if (new_buf) {
					buf = new_buf;
					bufsize *= 2;
				}
			}
			bytes_written += fread(buf + bytes_written, 1, bufsize - bytes_written - 1, fp);
		} while (!WIFEXITED(status) && !WIFSIGNALED(status));
		buf[bytes_written] = '\0';
		fclose(fp);

		struct discord_attachment attachment = {
			.content = buf,
			.filename = path,
			.content_type = "text/ansi"
		};

		struct discord_interaction_response res = {
			.type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
			.data = &(struct discord_interaction_callback_data) {
				.content = msg,
				.attachments = &(struct discord_attachments) {
					.size = 1,
					.array = &attachment
				}
			}
		};

		discord_create_interaction_response(client, event->id, event->token, &res, NULL);
		free(buf);
	}
}

int main(void) {
	struct discord *client = discord_config_init("config.json");
	discord_set_on_ready(client, &on_ready);
	discord_set_on_interaction_create(client, &on_interaction);
	discord_run(client);
	discord_cleanup(client);
	ccord_global_cleanup();
}
