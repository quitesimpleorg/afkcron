/*
 * afkcron launches a script depending on the time the user is not using his machine
*
 * Copyright (c) 2014-2018 Albert S. <launchutils at quitesimple dot org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <X11/Xlib.h>
#include <X11/extensions/scrnsaver.h>

bool strempty(const char *str)
{
	return str == NULL || *str == '\0';
}

void print_err_and_exit(const char *s)
{
	fputs(s, stderr);
	exit(EXIT_FAILURE);
}

void perror_and_exit(const char *s)
{
	perror(s);
	exit(EXIT_FAILURE);
}




int get_idle_seconds(Display *display)
{
	XScreenSaverInfo info;
	if(XScreenSaverQueryInfo(display, DefaultRootWindow(display), &info) == 0)
	{
		return -1; 
	}
	return info.idle / 1000; 
}

void child_handler(int signum, siginfo_t *info, void *context)
{
	if(signum != SIGCHLD) 
	{
		return;
	}
	int status;
	int ret = waitpid(info->si_pid, &status, WNOHANG);
	if(ret == -1)
	{
		perror_and_exit("waitpid failed");
	}
	if(status == EXIT_FAILURE)
	{
		print_err_and_exit("script did not succeed, exiting");
	}

}

void set_signals()
{
	struct sigaction action;
	action.sa_flags = SA_NOCLDSTOP | SA_SIGINFO;
	action.sa_sigaction = &child_handler;
	if(sigaction(SIGCHLD, &action, NULL) == -1)
	{
		perror_and_exit("sigaction failed");
	}
}

void run_script(const char *path, const char *type, int sleepseconds)
{
	pid_t child = fork();
	if(child == 0)
	{
		char sleepstr[20];
		snprintf(sleepstr, sizeof(sleepstr), "%i", sleepseconds);
		const char *args[4];
		args[0] = path;
		args[1] = type;
		args[2] = sleepstr;
		args[3] = NULL;
		execv(path, (char * const *)args);
		perror_and_exit("failed launching script");

	}
	if(child == -1)
	{
		perror_and_exit("fork failed");
	}
}

int main(int argc, char *argv[])
{
	if(argc < 3)
	{
		printf("Usage: afkcron script.sh interval\n");
		exit(EXIT_FAILURE);
	}
	
	const char *scriptpath = argv[1];
	unsigned int interval = strtol(argv[2], NULL, 10);
	
	Display *display = XOpenDisplay(NULL);
	if(display == NULL)
	{
		print_err_and_exit("Couldn't open DISPLAY");
	}
	
	int event_base, error_base;

	if (! XScreenSaverQueryExtension(display, &event_base, &error_base)) 
	{
		print_err_and_exit("No XScreenSaver Extension available on this display");
	}
	set_signals();
	


	int previous_seconds = 0;
	while(1)
	{
		int idle_seconds = get_idle_seconds(display);
		if(idle_seconds == -1)
		{
			print_err_and_exit("X11 Screen Saver Extension not supported?");
		}
		
		if(previous_seconds > idle_seconds)
		{
			run_script(scriptpath, "active", idle_seconds);
		}
		else
		{
			run_script(scriptpath, "idle", idle_seconds);
		}
		previous_seconds = idle_seconds;
		while(sleep(interval));
	}

}

