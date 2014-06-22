/*
 * processpool.h
 *
 *  Created on: 2014-5-18
 *      Author: fisheuler
 */

#ifndef PROCESSPOOL_H_
#define PROCESSPOOL_H_

#include<sys/types.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<assert.h>
#include<stdio.h>
#include<signal.h>
#include<unistd.h>
#include<errno.h>
#include<string.h>
#include<fcntl.h>
#include<stdlib.h>
#include<sys/epoll.h>
#include<sys/wait.h>
#include<sys/stat.h>

class process
{
public:
	process():m_pid(-1){}

public:
	pid_t m_pid;
	int m_pipefd[2];

};


/* 进程池类*/
template< typename T>
class processpoll
{
private:
	processpoll(int listenfd,int process_number =8);
public:
	static processpoll<T> * create(int listenfd,int process_number =8)
	{
		if(!m_instance)
		{
			m_instance = new processpoll<T>(listenfd,process_number);

		}
		return m_instance;
	}
	~processpoll()
	{
		delete [] m_sub_process;
	}

	void run();


private:
	void setup_sig_pipe();
	void run_parent();
	void run_child();

private:
	static const int MAX_PROCESS_NUMBER = 16;
	static const int USER_PER_PROCESS = 65536;
	static const int MAX_EVENT_NUMBER = 10000;
	int m_process_number;

	int m_idx;
	int m_epollfd;
	int m_listenfd;
	int m_stop;
	process* m_sub_process;

	static processpoll<T>* m_instance;

};


template<typename T>
processpoll<T>* processpoll<T>::m_instance =NULL;

static int sig_pipefd[2];


static int setnonblocking(int fd){
	int old_option = fcntl(fd,F_GETFL);

	int new_option = old_option | O_NONBLOCK;

	fcntl(fd,F_SETFL,new_option);

	return old_option;
}


static void addfd(int epoll_fd, int fd)
{
	struct epoll_event event;
	event.data.fd = fd;
	event.events = EPOLLIN | EPOLLET | EPOLLERR;
	epoll_ctl(epoll_fd,EPOLL_CTL_ADD,fd,&event);
	setnonblocking(fd);
}

static void removefd(int epollfd,int fd)
{
	epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
	close(fd);
}


static void sig_handler(int sig){

	int save_errno = errno;
	int msg = sig;
	send(sig_pipefd[1],(char*) &msg,1,0);
	errno = save_errno;

}

void addsig(int sig,void(*handler)(int ),bool restart)
{

	struct sigaction sa;
	memset(&sa,'\0',sizeof(sa));
	sa.sa_handler = handler;
	if(restart)
	{
		// 0x10000000
//		sa.sa_flags |= SA_RESTART;
		sa.sa_flags |= 0x10000000;
	}

	sigfillset(&sa.sa_mask);
	assert(sigaction(sig,&sa,NULL) != -1);
}


template<typename T>
processpoll<T>::processpoll(int listenfd,int process_number)
	:m_listenfd(listenfd),m_process_number(process_number),m_idx(-1),m_stop(false)
	 {
		assert( (process_number>0) && (process_number<=MAX_PROCESS_NUMBER));

		m_sub_process = new process[process_number];
		assert(m_sub_process);

		int i=0;
		for(;i<process_number;i++)
		{
			int ret = socketpair(PF_UNIX,SOCK_STREAM,0,m_sub_process[i].m_pipefd);
			assert(ret == 0);

			m_sub_process[i].m_pid = fork();

			assert(m_sub_process[i].m_pid>=0);
			if(m_sub_process[i].m_pid > 0)
			{
				close(m_sub_process[i].m_pipefd[1]);
				continue;
			}
			else
			{
				close(m_sub_process[i].m_pipefd[0]);
				m_idx = i;
				break;
			}
		}


	 }


template<typename T>
void processpoll<T>::setup_sig_pipe()
{
	/*创建监听事件和信号管道*/
	m_epollfd =epoll_create(5);

	assert(m_epollfd != -1);

	int ret = socketpair(PF_UNIX,SOCK_STREAM,0,sig_pipefd);
	assert( ret != -1);

	setnonblocking(sig_pipefd[1]);
	addfd(m_epollfd,sig_pipefd[0]);

	addsig(SIGCHLD,sig_handler,true);
	addsig(SIGTERM,sig_handler,true);
	addsig(SIGINT,sig_handler,true);
	addsig(SIGPIPE,SIG_IGN,true);
}

template<typename T>
void processpoll<T>::run()
{
	if(m_idx != -1)
	{
		run_child();
		return;
	}
	run_parent();

}

template<typename T>
void processpoll<T>::run_child()
{
	setup_sig_pipe();

	//每个子进程通过m-idx找到与父进程通信的管道
	int pipefd = m_sub_process[m_idx].m_pipefd[1];

	/*子进程需要监听pipefd，因为父进程通过它来通知子进程accept新链接*/

	addfd(m_epollfd,pipefd);

	struct epoll_event events[MAX_EVENT_NUMBER];

	T* users = new T [USER_PER_PROCESS];

	assert(users);

	int number =0;
	int ret =-1;

	while(!m_stop)
	{
		number = epoll_wait(m_epollfd,events,MAX_EVENT_NUMBER,-1);
		if((number <0)&&(errno != EINTR))
		{
			printf("epoll failure\n");
			break;
		}
		int i=0;
		for(;i<number;i++)
		{
			int sockfd = events[i].data.fd;

			if((sockfd == pipefd) && (events[i].events&EPOLLIN))
			{
				int client =0;


				/*从父子进程的管道中读取数据*/
				ret = recv(sockfd,(char*)&client,sizeof(client),0);

				if(((ret < 0) && (errno != EAGAIN) )|| ret == 0)
				{
					continue;
				}
				else
				{
					struct sockaddr_in client_address;
					socklen_t client_addrlength = sizeof(client_address);
					int connfd = accept(m_listenfd,(struct sockaddr*)&client_address,&client_addrlength);

					if(connfd <0)
					{
						printf("errno is :%d\n",errno);
					}

					addfd(m_epollfd,connfd);

					/*模板T需要实现init方法,以初始化一个客户端链接*/
					users[connfd].init(m_epollfd,connfd,client_address);

				}

			}
			//子进程收到的信号
			else if((sockfd == sig_pipefd[0]) &&( events[i].events&EPOLLIN))
			{
				int sig;

				char signals[1024];

				ret = recv(sig_pipefd[0],signals,sizeof(signals),0);
				if(ret <=0)
				{
					continue;
				}
				else
				{
					int i =0;
					for(;i<ret;++i)
					{
						switch(signals[i])
						{
							case SIGCHLD:
							{
								pid_t pid;
								int stat;
								while((pid = waitpid(-1,&stat,WNOHANG))>0)
								{
									continue;
								}

								break;
							}
							case SIGTERM:
							case SIGINT:
							{
								m_stop = true;
								break;
							}
							default:
							{
								break;
							}
						}
					}
				}


			}
			else if (events[i].events&EPOLLIN)
			{
				users[sockfd].process();
			}
			else
			{
				continue;
			}

		}
	}

	delete [] users;
	users = NULL;
	close(pipefd);

	// close(m_listenfd)

	close(m_epollfd);

}

template<typename T>
void processpoll<T>::run_parent()
{
	setup_sig_pipe();

	addfd(m_epollfd,m_listenfd);

	struct epoll_event events[MAX_EVENT_NUMBER];
	int sub_process_counter =0;

	int new_conn = 1;

	int number =0;
	int ret =-1;

	while(!m_stop)
	{
		number = epoll_wait(m_epollfd,events,MAX_EVENT_NUMBER,-1);
		if((number<0) && (errno != EINTR))
		{
			printf("epoll failure\n");
			break;
		}
		int i=0;
		for(;i<number;i++)
		{
			int sockfd = events[i].data.fd;
			if(sockfd == m_listenfd)
			{
				/* 采用round robin的方式将其分配给子进程处理*/
				int i = sub_process_counter;

				do
				{
					if(m_sub_process[i].m_pid != -1)
					{
						break;
					}
					i=(i+1)%m_process_number;
				}while(i != sub_process_counter);

				if(m_sub_process[i].m_pid == -1)
				{
					m_stop = true;
					break;
				}

				sub_process_counter = (i+1)%m_process_number;

				send(m_sub_process[i].m_pipefd[0],(char*)&new_conn,sizeof(new_conn),0);

				printf("send request to child %d\n",i);
			}
			//处理父进程接受的信号
			else if( (sockfd ==sig_pipefd[0]) && (events[i].events&EPOLLIN))
			{
				int sig;
				char signals[1024];
				ret = recv(sig_pipefd[0],signals,sizeof(signals),0);

				if(ret <= 0)
				{
					continue;
				}

				else
				{
					int i=0;
					for(;i<ret;i++)
					{
						switch(signals[i])
						{
							case SIGCHLD:
							{
								pid_t pid;
								int stat;
								while((pid=waitpid(-1,&stat,WNOHANG))>0)
								{
									int i=0;
									for(;i<m_process_number;++i)
									{
										/*如果进程池中第i个进程退出，主进程关闭相应的通信管道，置m_pid为-1，标志此子进程已经退出
										 *
										 */
										if(m_sub_process[i].m_pid == pid)
										{
											printf("child %d join\n",i);
											close(m_sub_process[i].m_pipefd[0]);
											m_sub_process[i].m_pid = -1;
										}
									}
								}

								// all child process exits, as parent process we exits too
								m_stop = true;
								int i=0;
								for(;i<m_process_number;++i)
								{
									if(m_sub_process[i].m_pid != -1)
									{
										m_stop = false;
									}
								}

								break;

							}
							case SIGTERM:
							case SIGINT:
							{
								/* parent process receive terminate signal ,just kill all child process
								 *
								 */
								printf("kill all the child now\n");
								int i=0;
								for(;i<m_process_number;++i)
								{
									int pid = m_sub_process[i].m_pid;
									if(pid != -1)
									{
										kill(pid,SIGTERM);
									}
								}

								break;
							}
							default:
							{
								break;
							}

						}
					}
				}

			}
			else
			{
				continue;
			}

		}
	}

	// close(m_listenfd)  should the new of this object to close this

	close(m_epollfd);

}
#endif /* PROCESSPOOL_H_ */
