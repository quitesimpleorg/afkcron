/*
 * afkcron launches a program depending on the time the user is not using his machine
*
 * Copyright (c) 2014-2017 Albert S. <launchutils at quitesimple dot org>
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
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <X11/Xlib.h>
#include <X11/extensions/scrnsaver.h>
#include <ctype.h>

#define CONFIG_DEFAULT_PATH "/etc/afkcron"
#define CONFIG_DELIMER ':'

//return actions flags
#define FLAG_RETURN_KILL (1 << 0)
#define FLAG_RETURN_PRIO (1 << 1)
#define FLAG_RETURN_STOP (1 << 2)
#define FLAG_RETURN_STAY (1 << 3)
#define FLAG_RETURN_TERM (1 << 4)
#define FLAG_RETURN_TERMKILL (FLAG_RETURN_TERM | FLAG_RETURN_KILL)
//general flags
#define FLAG_SINGLE_SHOT (1 << 0) // fire only once during program lifetime

//consider merging return action flags and general flags
struct entry 
{
	pid_t pid; // 0 = can be ran, -1 single shot entry which finished, pid > 0 = currently running
	bool stopped;
	const char *path;
	const char *args;
	int comeback_action; //What to do, when the user returns, starts using the computer again?
	int idle_seconds;
	int flags;
	struct entry *next;
};

FILE *logfp = NULL;

struct entry *head_entry = NULL; 
struct entry *tail_entry = NULL;

void *xmalloc(size_t n)
{
	char *tmp = malloc(n);
	if(tmp == NULL)
	{
		perror("malloc");
		exit(EXIT_FAILURE);
	}
	return tmp;
}

void *xstrdup(const char *str)
{
	char *result = strdup(str);
	if(result == NULL)
	{
		perror("strdup");
		exit(EXIT_FAILURE);
	}
	return result;
}

bool strempty(const char *str)
{
	return str == NULL || *str == '\0';
}

int strcnt(const char *str, const char c)
{
	int result = 0;
	while(*str)
		if(*str++ == c) ++result;
	return result; 
}

int split_string(char ***out, const char *str, char delim)
{
	int current = 0;
	int items = strcnt(str, delim) + 1;
	*out = xmalloc(items * sizeof(char *));
	char *temp = xstrdup(str);
	char *portion = temp;
	while(*temp)
	{
		if(*temp == delim) 
		{
			*temp=0;
			(*out)[current] = portion;
			++current;
			portion = temp+1;
		}
		++temp;
	}
	(*out)[current] = portion;
	return ++current;
}

const char **create_execv_args(const char *name, const char *str, char delim)
{
	if(strempty(name))
		return NULL;
	const char **result = NULL;
	if(strempty(str))
	{
		result = xmalloc(2 * sizeof(char *));
		result[0] = name;
		result[1] = NULL;
	}
	else
	{
		int items=2;
		char **args = NULL;
		int n_args = split_string(&args, str, delim);
		items += n_args;
		result = xmalloc(items * sizeof(char *)); 
		result[0] = name;
		for(int i=0; i < n_args; i++)
			result[i+1] = args[i];
		result[items-1] = NULL;
	}
	return result;

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


void logit(const char *format, ...)
{
	if(logfp != NULL)
	{
		va_list args;
		va_start(args, format);
		time_t now = time(0);
		char *timestr = ctime(&now);
		size_t len = strlen(timestr);
		if(timestr[len-1] == '\n')
		{
			timestr[len-1] = 0;
		}
		fprintf(logfp, "%s: ", timestr);
		vfprintf(logfp, format, args);
		fflush(logfp);
		va_end(args);
	}

}

int get_idle_seconds(Display *display)
{
	XScreenSaverInfo info;
	if(XScreenSaverQueryInfo(display, DefaultRootWindow(display), &info) == 0)
		return -1; 
	return info.idle / 1000; 
}

void add_entry(struct entry *e)
{
	if(head_entry == NULL)
		head_entry = e;
	if(tail_entry != NULL)
		tail_entry->next = e;
	tail_entry = e;
}


int comeback_action_from_string(const char *str)
{
		int result = 0;
		if(! strempty(str))
		{
			if(strcmp(str, "kill") == 0)
				result |= FLAG_RETURN_KILL;
			else if(strcmp(str, "stop") == 0)
				result |= FLAG_RETURN_STOP;
			else if(strcmp(str, "prio") == 0)
				result |= FLAG_RETURN_PRIO;
			else if(strcmp(str, "term") == 0)
				result |= FLAG_RETURN_TERM;
			else if(strcmp(str, "termkill") == 0)
				result |= FLAG_RETURN_TERMKILL;
			else
				result |= FLAG_RETURN_STAY;
		}
		return result;
}

int flags_from_string(const char *str)
{
	int result = 0;
	if(strstr(str, "oneshot"))
		result |= FLAG_SINGLE_SHOT;
	return result;
}
//TODO: check integer overflow
int secs_from_string(char *unitstr)
{
	if(unitstr == NULL) 
		return -1;
	size_t len = strlen(unitstr);
	char unit = unitstr[len-1];
	if(! isdigit(unit))
		unitstr[len-1] = '\0';
	
	int secs = atoi(unitstr);
	if(secs < 0)
		return -1;
	
	switch(unit)
	{
			case 'd':
				secs *= 24;
			case 'h':
				secs *= 60;
			case 'm':
				secs *= 60;
	}
	
	return secs;
}

struct entry *entry_from_line(const char *line)
{
	char *l = xstrdup(line);
	
	char **fields = NULL;
	int n_fields = split_string(&fields, l, CONFIG_DELIMER);
	if(n_fields < 5)
		return NULL;

	struct entry *result = xmalloc(sizeof(struct entry));
	result->path = fields[0];
	result->args = fields[1];
	result->comeback_action = comeback_action_from_string(fields[2]);
	result->idle_seconds = secs_from_string(fields[3]);
	result->flags = flags_from_string(fields[4]);
	return result;

}


static inline bool check_entry(struct entry *e)
{
	return ( ! strempty(e->path) ) && ( e->idle_seconds > 0 ) && ( e->comeback_action > 0 ); 
}

void read_config(const char *configfile)
{
	FILE *fp = fopen(configfile, "r");
	if(fp == NULL)
	{
		perror_and_exit("fopen");
	}	
	char *line;
	size_t n = 0;
	ssize_t r;
	while((r = getline(&line, &n, fp)) != -1 )
	{
		if(line[r-1] == '\n') 
			line[r-1] = '\0';
	
		struct entry *e = entry_from_line(line);
		if(e == NULL)
			print_err_and_exit("error reading from file");
				
		if(! check_entry(e))
			print_err_and_exit("Invalid values for entry");
		
		add_entry(e);
		
		
	}
	
	if(ferror(fp))
		perror_and_exit("error reading from config file");
	fclose(fp);
}

void handle_comeback()
{
	for(struct entry *current = head_entry; current != NULL; current = current->next)
	{
		if(current->pid > 0)
		{
			int comeback_action = current->comeback_action;
			bool wants_term = (comeback_action & FLAG_RETURN_TERM) == FLAG_RETURN_TERM;	
			
			if(wants_term)
			{
				logit("Terminating %z\n", current->pid);
				kill(current->pid, SIGTERM);
			}
		
			if(comeback_action & FLAG_RETURN_KILL)
			{
				if(wants_term)
					while(sleep(2));
				if(kill(current->pid, SIGKILL) == 0)
				{
					logit("Killed %z\n", current->pid);
					current->pid = 0;
				}
				//TODO: and if fail?...
			}
			if(comeback_action & FLAG_RETURN_STOP)
			{
				if(kill(current->pid, SIGSTOP) == 0)
				{
					logit("Stopped %z\n", current->pid);
					current->stopped = true;
					//TODO: ...
				}
			}
			
			if(comeback_action & FLAG_RETURN_PRIO)
			{
				logit("Lowering priority for %z\n", current->pid);
				setpriority(PRIO_PROCESS, current->pid, 19);
				//TODO: IO prio?
			}
		}
		
	}
}

void handle_finished_pid(pid_t pid)
{
	for(struct entry *current = head_entry; current != NULL; current = current->next)
	{
		if(current->pid == pid)
		{
			if( (current->flags & FLAG_SINGLE_SHOT) )
			{
				current->pid = -1;
			}
			else
				current->pid = 0;
		}
	}
}


void child_handler(int signum, siginfo_t *info, void *context)
{
	if(signum != SIGCHLD) 
		return;
		
	pid_t pid = info->si_pid;
	//TODO
	int status;
	int x = waitpid(pid, &status, WNOHANG);
	if(x == -1)
		print_err_and_exit("waitpid failed");
	handle_finished_pid(pid);
	
	
}
void set_signals()
{
	struct sigaction action;
	action.sa_flags = SA_NOCLDSTOP | SA_SIGINFO;
	action.sa_sigaction = &child_handler;
	if(sigaction(SIGCHLD, &action, NULL) == -1)
		print_err_and_exit("sigaction failed");
}


bool run_entry(struct entry *e)
{
	const char **args = create_execv_args(e->path, e->args, ' ');
	pid_t child = fork();
	if(child == 0)
	{
		logit("Starting execution of %s\n", e->path);
		execv(e->path, (char * const *)args);
		fclose(logfp);	
	}
	else if(child > 0)
		e->pid = child;
	return child != -1;
}

void run_entries(int idle_seconds)
{
	for(struct entry *e = head_entry; e != NULL; e = e->next)
	{
		if(e->idle_seconds > idle_seconds) 
			continue; 
		if(e->pid == 0)
			run_entry(e);
		if(e->pid > 0 && e->stopped)
		{
			logit("Continuing %z\n", e->pid);
			if(kill(e->pid, SIGCONT) == 0)
				e->stopped = false;
		
		}
		
	}
}



int main(int argc, char *argv[])
{
	Display *display = XOpenDisplay(NULL);
	if(display == NULL)
		print_err_and_exit("Couldn't open DISPLAY");

	
	int event_base, error_base;

	if (! XScreenSaverQueryExtension(display, &event_base, &error_base)) 
		print_err_and_exit("No XScreenSaver Extension available on this display");
		
	set_signals();
	



	int option;
	while((option = getopt(argc, argv, "c:l:")) != -1)
	{
		switch(option)
		{
			case 'c':
				read_config(optarg);
				break;
			case 'l':
				logfp = fopen(optarg, "a");
				if(logfp == NULL)
				{
					print_err_and_exit("Error opening log file");
				}
				break;

		}
	}
	if(head_entry == NULL)
	{
		read_config(CONFIG_DEFAULT_PATH); 
	}
	int previous_seconds = 0;
	while(1)
	{
		int idle_seconds = get_idle_seconds(display);
		if(idle_seconds == -1)
			print_err_and_exit("X11 Screen Saver Extension not supported?");

		if(previous_seconds > idle_seconds)
				handle_comeback();
		else
				run_entries(idle_seconds);
			
		previous_seconds = idle_seconds;
		while(sleep(10)); // Well this sleep approach is suboptimal, do it with events somehow if possible.
	}

}

