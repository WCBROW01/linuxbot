#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <concord/discord.h>
#include <concord/log.h>

#include "job.h"

// Function pointer type for commands
typedef void (*command_func)(struct discord *, const struct discord_interaction *);

struct bot_command {
	struct discord_create_global_application_command cmd;
	const command_func func;
};

#define P99_PROTECT(...) __VA_ARGS__

// absolutely ridiculous preprocessor hack.
#define _CREATE_OPTIONS(options) &(struct discord_application_command_options) { .size = sizeof((struct discord_application_command_option[]) options) / sizeof(struct discord_application_command_option), .array = (struct discord_application_command_option[]) options }
#define CREATE_OPTIONS(...) _CREATE_OPTIONS(P99_PROTECT(__VA_ARGS__))

static void bot_command_run(struct discord *client, const struct discord_interaction *event) {
	char *cmd = event->data->options->array[0].value;
	char msg[DISCORD_MAX_MESSAGE_LEN];

	job_uid_t uid = submit_job(
		cmd, event->user ? event->user->id : event->member->user->id, event->channel_id
	);

	if (uid == -1) {
		strncpy(msg, "There was a problem queuing your job. The queue may be full.", sizeof(msg));
	} else {
		snprintf(msg, sizeof(msg), "Your job has been queued!\nJob ID: `%lx`", uid);
	}

	struct discord_interaction_response res = {
		.type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
		.data = &(struct discord_interaction_callback_data) {
			.content = msg
		}
	};

	discord_create_interaction_response(client, event->id, event->token, &res, NULL);
}

static void bot_command_help(struct discord *client, const struct discord_interaction *event) {
	char msg[DISCORD_MAX_MESSAGE_LEN];

	// intro message
	snprintf(
		msg, sizeof(msg),
		"Hello %s, Welcome to Linux Bot! This is currently a work in progress, but many features are planned!\n"
		"You can find the source code for this bot at https://github.com/WCBROW01/linuxbot\n"
		"Please submit any bugs or issues there, or feel free to make a pull request!",
		event->user ? event->user->username : event->member->user->username
	);

	struct discord_interaction_response res = {
		.type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
		.data = &(struct discord_interaction_callback_data) {
			.content = msg
		}
	};

	discord_create_interaction_response(client, event->id, event->token, &res, NULL);
}

static struct bot_command commands[] = {
	{
		.cmd = {
			.name = "run",
			.description = "Start a new job.",
			.default_permission = true,
			.dm_permission = true,
			.options = CREATE_OPTIONS({
				{
					.type = DISCORD_APPLICATION_OPTION_STRING,
					.name  = "command",
					.description = "Your command",
					.required = true
				}
			})
		},
		.func = &bot_command_run
	},
	{
		.cmd = {
			.name = "help",
			.description = "Get help on how to use the bot!"
		},
		.func = &bot_command_help
	}
};

static void on_ready(struct discord *client, const struct discord_ready *event) {
	log_info("Logged in as %s!", event->user->username);
	init_job_queue(client);

	// create commands
	for (struct bot_command *i = commands; i < commands + sizeof(commands) / sizeof(*commands); ++i) {
		discord_create_global_application_command(client, event->application->id, &i->cmd, NULL);
	}
}

static void on_interaction(struct discord *client, const struct discord_interaction *event) {
	if (event->type != DISCORD_INTERACTION_APPLICATION_COMMAND)
		return; // not a slash command

	// invoke the command
	for (const struct bot_command *i = commands; i < commands + sizeof(commands) / sizeof(*commands); ++i) {
		if (!strcmp(event->data->name, i->cmd.name)) {
			i->func(client, event);
			return;
		}
	}

	// not a real command
	struct discord_interaction_response res = {
		.type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
		.data = &(struct discord_interaction_callback_data) {
			.content = "Invalid command, contact the maintainer of this bot."
		}
	};
	discord_create_interaction_response(client, event->id, event->token, &res, NULL);
}

int main(void) {
	srandom(time(NULL));
	struct discord *client = discord_config_init("config.json");
	discord_set_on_ready(client, &on_ready);
	discord_set_on_interaction_create(client, &on_interaction);
	discord_run(client);
	discord_cleanup(client);
	ccord_global_cleanup();
}
