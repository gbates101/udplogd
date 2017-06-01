/****************************************************
 * udplogd - Simple udp message logger
 *
 * This program creates a daemon listening
 * for udp messages and writting them to
 * disk as they come in.
 *
 * Author: Garrett Bates <gbates1@kent.edu>
 *
 * Date: 4/30/2014
 * Version: 1.0
 *
 * Original Author: Doug Stanley <dmstanle@kent.edu>
 *****************************************************/

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include "udplogd.h"

/* For the signal handler function to successfully manage a clean shutdown, 
 * it needs access to the following globally-declared variables.
 * Since signal handler functions do not support parameters, making the
 * log file, socket, and pthread mutex/IDs global was the best solution.
 */
pthread_t tarry[NTHREADS];
pthread_mutex_t gary;
FILE *log_file;
int socket_fd;


void *udp_printer(void *arg) {
	unsigned char buff[MAX_MSG_BUFF] = {0};	
	int nbytes = 0;
	int oldstate;	/* In order to comply with POSIX standards, a buffer
					 * must be provided in the call to pthread_setcancelstate()
					 * in place of using NULL.
					 */

	while (1) {
		// Block until a UDP datagram is read.
		// fprintf(stdout, "nbytes = %d", nbytes);
		nbytes = recvfrom(socket_fd, buff, MAX_MSG_BUFF, 0, NULL, NULL);

		/* Be sure to set the cancel state to disabled before
		 * locking and writing, otherwise deadlock could possibly 
		 * occur with a thread that cancels after locking.
		 */
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate);
		pthread_mutex_lock(&gary);	

		// Since stdout is redirected to my log file, write to stdout.	
		fwrite(buff, sizeof(unsigned char), nbytes, stdout);
		fflush(stdout);

		// Unlock, then make the thread cancelable.
		pthread_mutex_unlock(&gary);
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate);
	}
}

void stop_lissenin(int sig) {
	int i;

	// Close the socket first, so no more packets are received.
	close(socket_fd);

	// Cancel all threads.
	for( i = 0; i < NTHREADS; i++ ){
		pthread_cancel( tarry[i] );
	}

	// Join all threads.
	for( i = 0; i < NTHREADS; i++ ){
		pthread_join( tarry[i], NULL );	
	}	

	// Destroy mutex after joining threads.
	pthread_mutex_destroy( &gary );

	// Write any remaining buffered input to file.
	// NOTE: Passing NULL to fflush() will flush all open streams.
	fflush(NULL);

	// Close log file.
	fclose(log_file);

	// Terminate the process.
	exit(EXIT_SUCCESS);
}

int main(int argc, char **argv){
    pid_t procid, sid;
    int ret = 0;
	FILE *log_file, *pid_file;
	struct stat st; 
	struct sockaddr_in procsock;


	/* Initialize sockaddr_in struct. */
	memset(&procsock, 0, sizeof(struct sockaddr_in));
	procsock.sin_family = AF_INET;
	procsock.sin_addr.s_addr = INADDR_ANY;
	procsock.sin_port = htons(UDP_LOGGER_PORT);

	/* Open socket to receive socket descriptor. */
	socket_fd = socket(AF_INET, SOCK_DGRAM, 0);

	/* Bind to opened socket. */
	ret = bind(socket_fd, (const struct sockaddr *)&procsock, sizeof(procsock));
	if ( ret != 0 ) {
		fprintf(stderr, "Binding to socket %d failed: Is it already bound?\n", UDP_LOGGER_PORT);
		exit(EXIT_FAILURE);
	}
	
	/* Check for the existence of a udplogd.pid file. 
	 * If it doesn't exist, open it.
	 * Otherwise, exit with error.
	 */
	if ( stat("udplogd.pid", &st) != 0 ) {
		pid_file = fopen("udplogd.pid", "w");
	} else {
		perror("Opening PID file failed: File already exists!\n");
		close(socket_fd);
		exit(EXIT_FAILURE);	
	}

	/* Fork process AFTER binding to socket. 
	 * This allows the above steps to be testable. 
	 */
	procid = fork();
	//printf("procid = %d", procid);

    if ( procid < 0 ) {
        perror("Fork Failed!");
		close(socket_fd);
        exit(EXIT_FAILURE);
    } else if( procid > 0 ) {
        /* Have parent exit */
		exit(EXIT_SUCCESS);
    }
	
    /* Adjust umask, so that logfiles
     * are readable by anyone.
     */
    umask(022);

    /* Get our pid and write it to a file */
    procid = getpid();
    
	/* Create/open PID file. */
	
	// Write PID to PID file.
	fprintf(pid_file, "%d", procid);

	// Close PID file stream after writing.
	fclose(pid_file);

    /* Open a stream to the log file in append mode */
	log_file = fopen("udplogd.log", "a");

	/* Ensure that log file was opened successfully for writing. */
	if ( log_file == NULL ) {
		perror("Could not open log file.\n");
		exit(EXIT_FAILURE);
	}

    /* Redirect STDERR to log */
    /* see dup() */
    
	freopen("udplogd.log", "a", stderr);
	freopen("udplogd.log", "a", stdout);
    
	/* Close STDIN last, as we won't need it */
    close(STDIN_FILENO);

    /* Create new session and become session
     * and process group leader
     */
    sid = setsid();
    if( sid < 0 ){
        perror("Setsid failed\n");
        exit(EXIT_FAILURE);
    }

    /* Change our working directory to
     * a directory that should always exist.
     * '/' is a good example.
     */
    if ((chdir("/")) < 0) {
        perror("Failed to chdir to /\n");
        exit(EXIT_FAILURE);
    }

	/* We register the signal handler for
     * SIGTERM to call shutdown() when received.
	 */
	struct sigaction shandle;
	sigset_t smask;
	sigset_t pmask;

	// Load all signals into mask.
	sigfillset(&smask);

	// Configure the handler before assigning it to SIGTERM
	shandle.sa_handler = (void *)stop_lissenin;
	shandle.sa_mask = smask;

	// Register function with SIGTERM signal.
	sigaction(SIGTERM, &shandle, NULL);

	// Create the proces mask to block all signals but SIGTERM.
	sigfillset(&pmask);
	sigdelset(&pmask, SIGTERM);

	// Set the process mask for our multi-threaded application
	pthread_sigmask(SIG_SETMASK, &pmask, NULL);

	// Initialize variables to create threads.
	int i, status = 0;

	pthread_mutex_init( &gary, NULL );

	
    /* Now we do the main work that our
     * daemon is meant to do. For starters,
     * we create the worker threads to
     * listen for UDP traffic, and
     * write it to a log file (via udp_printer()).
     */
	for( i = 0; i < NTHREADS; i++ ){
		status = pthread_create( &tarry[i], NULL, (void *)udp_printer, NULL );
		if(status != 0){
			perror("Failed to create thread.\n");	
			exit(EXIT_FAILURE);
		}
	}

	// We wait forever so signals can be handled.
	while(1) {
		sleep(1);
	}

	// Just in case things get to this point (looks like they dont).
	//close(sockfd);
	//close(log_fd);

	return 0;
}

