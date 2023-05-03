#ifndef LINUXBOT_JOB_H
#define LINUXBOT_JOB_H

typedef long job_uid_t;

void init_job_queue(struct discord *client);

job_uid_t submit_job(char *cmd, u64snowflake user_id, u64snowflake channel_id);

#endif
