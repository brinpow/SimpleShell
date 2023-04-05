#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "config.h"
#include "siparse.h"
#include "utils.h"
#include "builtins.h"

#define BUFFOR_SIZE 2*MAX_LINE_LENGTH+1
#define MAX_ZOMBIE_STATUS 4000
#define MAX_STATUS_LENGTH 80

typedef struct child_process
{
    pid_t pid;
    int present;
} child_process;


char buf[BUFFOR_SIZE];
size_t buffor_len=0;
pipelineseq * ln;
int to_continue=0;
int last_skip=0;

volatile child_process * foreground_children;
volatile int fg_children_count=0;
volatile int fg_children_cur=0;

char bg_messages[MAX_ZOMBIE_STATUS][MAX_STATUS_LENGTH];
volatile int bg_mess_count=0;

int is_terminal=0;

sigset_t child_mask;
sigset_t sigint_child_mask;
sigset_t empty_mask;

int remove_if_contains(pid_t pid)
{
    if(fg_children_count>0)
    {
        for(int i=0;i<fg_children_cur;i++)
        {
            if(foreground_children[i].present && foreground_children[i].pid==pid)
            {
                foreground_children[i].present=0;
                return 1;
            }
        }
    }
    return 0;
}

void child_handler(int sig_nb)
{
    pid_t childpid;
    do
    {
        int status; 
        childpid=waitpid(-1,&status,WNOHANG);
        if(childpid>0 && remove_if_contains(childpid))
        {
            fg_children_count--;        
        }
        else if(childpid>0)
        {
            if(is_terminal && bg_mess_count<MAX_ZOMBIE_STATUS)
            {
                if(WIFEXITED(status))
                {
                    int exit_status = WEXITSTATUS(status);
                    sprintf(bg_messages[bg_mess_count++],"Background process %d terminated. (exited with status %d)",childpid,exit_status);
                }
                else if(WIFSIGNALED(status))
                {
                    int signal_number = WTERMSIG(status);
                    sprintf(bg_messages[bg_mess_count++],"Background process %d terminated. (killed by signal %d)",childpid,signal_number);
                }
           }
       }
    } while (childpid>0);
}

void sigint_handler(int sig_nb)
{
    if(is_terminal)
    {
        char* endl = "\n";
        write(1,endl,1);
        write(1,PROMPT_STR,strlen(PROMPT_STR));
    }
}

static void print_error(int rv)
{
    if(rv==-1)
    {
        fprintf(stderr,"%s\n",SYNTAX_ERROR_STR);
        exit(EXIT_FAILURE);
    }
}

static void error_service(char * cname, int rv)
{
    if(rv==-1)
    {
        switch (errno)
        {
            case ENOENT:
                fprintf(stderr,"%s: no such file or directory\n",cname);
                break;
            case EACCES:
                fprintf(stderr,"%s: permission denied\n",cname);
                break;
            default:
                fprintf(stderr,"%s: exec error\n", cname);
                break;
        }
        exit(EXEC_FAILURE);
    }
}

static void closer(int oldfd)
{
    int rv;
    do
    {
        rv=close(oldfd);
    } while (rv==-1 && errno==EINTR);
    print_error(rv);
}


static void redirs_changer(int oldfd,int flags, mode_t mode, char * filename)
{
    closer(oldfd);
    int rv;
    do
    {
        rv = open(filename,flags,mode);
    } while (rv==-1 && errno==EINTR);

    error_service(filename, rv);
}

static void redirs_checker(command * com)
{
    redirseq * rd = com->redirs;
    if(rd!=NULL)
    {
        do
        {
            if(IS_RIN(rd->r->flags))
            {
                redirs_changer(0,O_RDONLY,0,rd->r->filename);
            }
            if(IS_ROUT(rd->r->flags))
            {
                redirs_changer(1,O_WRONLY|O_TRUNC|O_CREAT,S_IRWXU|S_IRWXG|S_IRWXO, rd->r->filename);
            }
            if(IS_RAPPEND(rd->r->flags))
            {
                redirs_changer(1,O_APPEND|O_WRONLY|O_CREAT,S_IRWXU|S_IRWXG|S_IRWXO,rd->r->filename);
            }
            rd=rd->next;
        } while(rd!=com->redirs);
    }
}

static void wait_fg_children()
{
    sigprocmask(SIG_BLOCK,&child_mask,NULL);
    do
    {
        sigsuspend(&empty_mask);
    } while (fg_children_count>0);
    sigprocmask(SIG_UNBLOCK,&child_mask,NULL);
}

void print_bg_messages()
{
    sigprocmask(SIG_BLOCK,&child_mask,NULL);
    for(int i=0;i<bg_mess_count;i++)
    {
        printf("%s\n",bg_messages[i]);
    }
    bg_mess_count=0;
    sigprocmask(SIG_UNBLOCK,&child_mask,NULL);
}


static int run_command(command * com,int index)
{
    argseq* x=com->args->next;
    int i=1;
    while(x!=com->args)
    {
        i++;
        x=x->next;
    }
    char* argv[i+1];
    x=com->args;
    for(int j=0;j<i;j++)
    {
        argv[j]=x->arg;
        x=x->next;
    }
    argv[i]=NULL;
    if(index>=0)
    {
        return (*builtins_table[index].fun)(argv);
    }
    else
    {
        redirs_checker(com);
        return execvp(com->args->arg,argv);
    }
}


static void parse_data(char * rptr)
{
    int dif=rptr-buf;
    if(dif>=MAX_LINE_LENGTH)
    {
        last_skip=1;
        to_continue=1;
    }
    buffor_len= buffor_len-dif-1;
    buf[dif]=0;

    if(!last_skip)
    {
        ln = parseline(buf);
    }
    else
    {
        fprintf(stderr,"%s\n",SYNTAX_ERROR_STR);
        last_skip=0;
    }

    memmove(buf,rptr+1,buffor_len);
    memset(buf+buffor_len,0,dif+1);
}

static int readf(char* buf, int size)
{
    int read_byte=1;
    do
    {
        read_byte=read(0,buf,size);
    } while(read_byte==-1 && errno==EINTR);

    print_error(read_byte);
    buffor_len=buffor_len+read_byte;
    return read_byte;
}

static int read_line()
{
    ssize_t read_byte=1;
    if(buffor_len==0)
    {
        read_byte = readf(buf,BUFFOR_SIZE-1);
    }

    char * rptr = memchr(buf,'\n',buffor_len);
    if(rptr==NULL)
    {
        if(buffor_len>=MAX_LINE_LENGTH)
        {
            buffor_len=0;
            to_continue=1;
            last_skip=1;
            memset(buf,0,BUFFOR_SIZE);
        }
        else if(buffor_len>0)
        {
            read_byte = readf(buf+buffor_len,MAX_LINE_LENGTH);

            if(read_byte==0)
            {
                parse_data(buf+buffor_len);
                buffor_len=0;
            }
            else
            {
                rptr = memchr(buf,'\n',buffor_len);
                if(rptr==NULL)
                {
                    to_continue=1;
                }
                else
                {
                    parse_data(rptr);
                }
            }
        }
        else
        {
            to_continue=1;
        }
    }
    else
    {
        parse_data(rptr);
    }

    return read_byte;
}

static int check_in_builtin(char * cname)
{
    int i=0;
    while(builtins_table[i].name!=NULL)
    {
        if(!strcmp(builtins_table[i].name,cname))
        {
            return i;
        }
        i++;
    }
    return -1;
}

static void pipe_changer(int oldfd, int newfd)
{
    if(oldfd!=newfd)
    {
        closer(newfd);
        int rv;
        do
        {
            rv=dup2(oldfd,newfd);
        } while (rv==-1 && errno==EINTR);

        print_error(rv);
        closer(oldfd);
    }
}

static void descriptor_closer(int fd, int dfd)
{
    if(fd!=dfd)
    {
        closer(fd);
    }
}

static void build_command(commandseq * cur_com,int in,int out, int readfd,int waits, int fg)
{
    int index=-1;
    char * cname=NULL;
    command * com=cur_com->com;
    if(com!=NULL)
    {
        cname=com->args->arg;
        index=check_in_builtin(cname);
        if(index==-1)
        {
            sigprocmask(SIG_BLOCK,&sigint_child_mask,NULL);
            pid_t child_pid=fork();
            if(child_pid==0)
            {
                if(!fg)
                {
                    setsid();
                }
                sigprocmask(SIG_UNBLOCK,&sigint_child_mask,NULL);

                pipe_changer(in,0);
                pipe_changer(out,1);
                descriptor_closer(readfd,0);
                int rv=run_command(com,index);
                error_service(cname,rv);
            }
            else if(child_pid>0)
            {
                descriptor_closer(in,0);
                descriptor_closer(out,1);
                if(fg)
                {
                    child_process child;
                    child.pid=child_pid;
                    child.present=1;
                    foreground_children[fg_children_cur++]=child;
                }
                sigprocmask(SIG_UNBLOCK,&sigint_child_mask,NULL);
                if(waits && fg)
                {
                    wait_fg_children();
                }
            }
            else
            {
                print_error(child_pid);
            }
        }
        else
        {
            if(run_command(com,index)==-1)
            {
                fprintf(stderr,"Builtin %s error.\n",cname);
            }
        }
    }
}

void create_sigaction(void (*handler)(int), struct sigaction* act, int signal)
{
	act -> sa_handler = handler;
	act -> sa_flags = 0;
	sigemptyset(&(act->sa_mask));
	sigaction(signal,act,NULL);
}

int main(int argc, char *argv[])
{
    struct stat fbuf;

    ssize_t read_byte=1;

    struct sigaction act_child;
    struct sigaction act_sigint;

    create_sigaction(child_handler,&act_child,SIGCHLD);
    create_sigaction(sigint_handler,&act_sigint,SIGINT);
    
    sigemptyset(&empty_mask);
    sigemptyset(&child_mask);
    sigemptyset(&sigint_child_mask);
    sigaddset(&child_mask,SIGCHLD);
    sigaddset(&sigint_child_mask,SIGINT);
    sigaddset(&sigint_child_mask,SIGCHLD);

    print_error(fstat(0,&fbuf));
    if(S_ISCHR(fbuf.st_mode))
    {
        is_terminal=1;
    }


    while (read_byte)
    {
        to_continue=0;
        if(is_terminal)
        {
            print_bg_messages();
            printf("%s",PROMPT_STR);
            fflush(stdout);
        }
        read_byte = read_line();

        if(ln==NULL && !to_continue)
        {
            fprintf(stderr,"%s\n",SYNTAX_ERROR_STR);
        }
        else if(!to_continue)
        {
            pipelineseq * cur = ln;
            do
            {
                int fg=1;
                commandseq * cur_com=cur->pipeline->commands;
                command * com = cur_com->com;

                int count=0;
                int null_com=0;
                int skip=0;

                do
                {
                    count++;
                    if(cur_com->com==NULL)
                    {
                        null_com++;
                    }
                    cur_com=cur_com->next;
                } while (cur_com!=cur->pipeline->commands);

                if(null_com && count>1)
                {
                    skip=1;
                }

                if(!skip)
                {
                    child_process processes[count];

                    sigprocmask(SIG_BLOCK,&child_mask,NULL);
                    fg_children_cur=0;
                    if(INBACKGROUND==cur->pipeline->flags)
                    {
                        fg=0;
                    }
                    else
                    {
                        fg_children_count=count;
                        foreground_children = processes;
                    }
                    sigprocmask(SIG_UNBLOCK,&child_mask,NULL);

                    if(cur_com->next==cur->pipeline->commands)
                    {
                        build_command(cur_com,0,1,0,1,fg);
                    }
                    else
                    {
                        int fd=0;
                        cur_com=cur_com->next;
                        do
                        {
                            int pipefd[2];
                            int rv= pipe(pipefd);
                            print_error(rv);
                            build_command(cur_com->prev,fd,pipefd[1],pipefd[0],0,fg);
                            fd=pipefd[0];
                            cur_com=cur_com->next;
                        } while (cur_com!=cur->pipeline->commands);

                        build_command(cur_com->prev,fd,1,0,1,fg);
                    }
                }
                else
                {
                    fprintf(stderr,"%s\n",SYNTAX_ERROR_STR);
                }
                cur=cur->next;
            } while (cur!=ln);
        }
    }

    return 0;

    /*ln = parseline("ls -las | grep k | wc ; echo abc > f1 ;  cat < f2 ; echo abc >> f3\n");
    printparsedline(ln);
    printf("\n");
    com = pickfirstcommand(ln);
    printcommand(com,1);

    ln = parseline("sleep 3 &");
    printparsedline(ln);
    printf("\n");

    ln = parseline("echo  & abc >> f3\n");
    printparsedline(ln);
    printf("\n");
    com = pickfirstcommand(ln);
    printcommand(com,1);
    return 0;*/
}
