#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h> //
#include <sys/wait.h> //

#include "debug.h"
#include "polya.h"

volatile sig_atomic_t done = 0;
volatile sig_atomic_t fail = 0;
sig_atomic_t worker_states[MAX_WORKERS];
sig_atomic_t worker_pid[MAX_WORKERS];
// started, idle, continued, running, stopped, exited, aborted
struct problem *prob;
struct result *res;
void *still;

int get_pid_index(int pid) {
    int i;
    for (i = 0; i < MAX_WORKERS; i++) {
        if (pid == worker_pid[i]) {
            return i;
        }
    }
    return -1;
}

// SIGCHLD HANDLER
// can be notified when worker processes stop and continue
void sigchld_handler(int sig) {
    // child: idle, running, stopped, exited, aborted
    debug("Received signal 17 (SIGCHLD)");

    pid_t pid;
    int status;
    //pid_t pid = waitpid(-1, &status, WSTOPPED | WCONTINUED | WNOHANG); // pid, status, options
    while ((pid = waitpid(-1, &status, WSTOPPED | WCONTINUED | WNOHANG)) > 0) {
        if (pid == -1) {
            perror("waitpid error");
            exit(EXIT_FAILURE);
        }
        int index = get_pid_index(pid);
        //debug("%s%d", "pid  ", pid);
        //debug("%s%d", "worker state ", worker_states[index]);
        // debug("%d", get_pid_index(pid));
        if (WIFSTOPPED(status)) {
            // debug("stopped");
            if (worker_states[index] == WORKER_STARTED) {
                // debug("made idle");
                // started -> idle
                worker_states[index] = WORKER_IDLE;
                sf_change_state(pid, WORKER_STARTED, WORKER_IDLE); // -> IDLE
            }
            else if (worker_states[index] == WORKER_RUNNING) {
                // debug("made stopped");
                // running -> stopped
                worker_states[index] = WORKER_STOPPED;
                sf_change_state(pid, WORKER_RUNNING, WORKER_STOPPED); // -> STOPPED
            } else {
                // debug("early");
                worker_states[index] = WORKER_STOPPED;
                sf_change_state(pid, worker_states[index], WORKER_STOPPED); // -> STOPPED
            }
        }
        else if (WIFCONTINUED(status)) { // -> RUNNING
            // debug("cont to run");
            // continued -> running
            worker_states[index] = WORKER_RUNNING;
            sf_change_state(pid, worker_states[index], WORKER_RUNNING);
        }
        else if (WIFEXITED(status)) { // -> EXITED
            // debug("worker exited caught");
            if (WEXITSTATUS(status) != 0) {
                debug("abort");
                worker_states[index] = WORKER_ABORTED;
                sf_change_state(pid, worker_states[index], WORKER_ABORTED);
                fail = 1;
            } else {
                worker_states[index] = WORKER_EXITED;
                sf_change_state(pid, worker_states[index], WORKER_EXITED);
                done = done + 1;
            }
        }
        else if (WIFSIGNALED(status)) { // check aborted
            debug("worker signaled exit caught");
            if (WTERMSIG(status) != 0) {
                debug("abort");
                worker_states[index] = WORKER_ABORTED;
                sf_change_state(pid, worker_states[index], WORKER_ABORTED);
            } else {
                worker_states[index] = WORKER_EXITED;
                sf_change_state(pid, worker_states[index], WORKER_EXITED);
            }
        }
        // debug("other");
    }
    // debug("EXIT HANDLER");
}

/*
 * master
 * (See polya.h for specification.)
 */
int master(int workers) {
    // TO BE IMPLEMENTED

    // void sf_start(void)
    // void sf_end(void)
    // void sf_change_state(int pid, int oldstate, int newstate)
    // void sf_send_problem(int pid, struct problem *prob)
    // void sf_recv_result(int pid, struct result *result)
    // void sf_cancel(int pid)
    // get_problem_variant and post_result

    sf_start();

    // INITIALIZATION

    // during initialization, each time the master process creates a worker process,
    // it creates two pipes for communicated with that worker process
    // one pipe for sending problems from master to worker
    // one pipe for sending results from worker to master

    // pipe(2)
        // before the worker process is forked
    // fork(2)
    // dup2(2)
        // worker/child redirects its standard input and standard output to the pipes
    // excel()
        // worker program is executed using one of the exec(3)

    // SIGPIPE HANDLER
    // so that it is not inadvertently terminated by the premature exit of a worker process
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) { // Install the handler
        perror("signal_error");
        exit(EXIT_FAILURE);
    }

    if (signal(SIGCHLD, sigchld_handler) == SIG_ERR) { // Install the handler
        perror("signal_error");
        exit(EXIT_FAILURE);
    }

    sigset_t mask_all;
    sigset_t prev_all;
    if (sigfillset(&mask_all) == -1) {
        perror("sigfillset error");
        exit(EXIT_FAILURE);
    }
    sigset_t mask_child;
    if (sigfillset(&mask_child) == -1) {
        perror("sigfillset error");
        exit(EXIT_FAILURE);
    }
    if (sigdelset(&mask_child, SIGCHLD) == -1) {
        perror("sigdelset error");
        exit(EXIT_FAILURE);
    }

    int pid;
    // fd[0] = read, fd[1] = write
    int send_problems[2];
    int send_results[2];
    int read_fd[workers];
    int write_fd[workers];
    FILE *in;
    FILE *out;

    // creates a number of worker processes (and associated pipes)
    // as specified by the workers paremeter

    int m;
    for (m = 0; m < workers; m++) {
        // debug("iterate through workers");

        // pipe for sending problems from master to worker
        if (pipe(send_problems) < 0) { // master to worker
            perror("Can't create pipe");
            exit(EXIT_FAILURE);
        }
        // pipe for sending results from worker to master
        if (pipe(send_results) < 0) { // worker to master
            perror("Can't create pipe");
            exit(EXIT_FAILURE);
        }

        read_fd[m] = send_results[0]; // master read results
        write_fd[m] = send_problems[1]; // master write problems


        if ((pid = fork()) == 0) { // CHILD
            debug("Started worker %d%s%d%s%d%s", m, " (in = ", send_problems[0], ", out = ", send_results[1], ")");

            // stdin = problems
            if (dup2(send_problems[0], 0) == -1) { // read problems written by master
                // oldfd, newfd
                perror("dup2 errorr");
                exit(EXIT_FAILURE);
            }
            // stout = reults
            if (dup2(send_results[1], 1) == -1) { // write results read by master
                perror("dup2 errorr");
                exit(EXIT_FAILURE);
            }

            if (close(send_problems[1]) == -1) { // close write problem
                perror("close error");
                exit(EXIT_FAILURE);
            }
            if (close(send_results[0]) == -1) { // close read results
                perror("close error");
                exit(EXIT_FAILURE);
            }

            debug("Starting worker %d%s%d%s", m, " (", pid, ")");

            if (execl("bin/polya_worker", "polya_worker", NULL) == -1) {
                perror("Worker execl erorr");
                exit(EXIT_FAILURE);
            }
        } else if (pid < 0) {
            perror("fork error");
            exit(EXIT_FAILURE);
        } else { // PARENT
            debug("Parent %d", m);

            if (close(send_problems[0]) == -1) { // close read problem
                perror("close error");
                exit(EXIT_FAILURE);
            }
            if (close(send_results[1]) == -1) { // close write reults
                perror("close error");
                exit(EXIT_FAILURE);
            }

            if (sigprocmask(SIG_BLOCK, &mask_all, &prev_all) < 0) { // block
                perror("sigprocmask");
                exit(EXIT_FAILURE);
            }
            worker_states[m] = WORKER_STARTED;
            sf_change_state(pid, 0, WORKER_STARTED);
            worker_pid[m] = pid;
            if (sigprocmask(SIG_SETMASK, &prev_all, NULL) < 0) { // unblock
                perror("sigprocmask");
                exit(EXIT_FAILURE);
            }

            // wait for master to get sigchld
            sigsuspend(&mask_child);
        }
    }

    //MAIN LOOP

    // PARENT
    debug("MASTER");

    // enters a main loop that repeatedly assign problems to idle workers
    // until all of the worker processes have become idle and
    // NULL is returned from get_problem_variant

    while(1) {
        if ((get_problem_variant(workers, 0)) != NULL) { // problems left
            // assign problem to idle workers
            for (int w = 0; w < workers; w++) {
                if ((get_problem_variant(workers, w)) == NULL) { // can happen in this for loop
                    // debug("no more");
                } else {
                    //debug("%d", w);
                    if (worker_states[w] == WORKER_IDLE) { // IDLE

                        if (sigprocmask(SIG_BLOCK, &mask_all, &prev_all) < 0) { // block
                            perror("sigprocmask");
                            exit(EXIT_FAILURE);
                        }

                        // CREATE PROBLEM

                        prob = get_problem_variant(workers, w);
                        // master process send a problem to the worker process
                        if ((out = fdopen(write_fd[w], "w")) == NULL) { // output
                            perror("Parent can't create output stream");
                            exit(EXIT_FAILURE);
                        }
                        // first writes a the fixed-size problem header to the pipe,
                        // and then continues by writing the problem data

                        // 1. read the header
                        fwrite(prob, sizeof(struct problem), 1, out);
                        //ferror
                        if (ferror(out)) {
                            perror("ferror");
                            exit(EXIT_FAILURE);
                        }
                        debug("Write problem");
                        // examine the size field of this structure and read an additional size
                        // 2. malloc storage for the full problem (header + data)
                        // 3. copy the already-read header into the beginning of the malloc'ed area
                        // prob = realloc(prob, prob->size); // realloc for new size (header + data)
                        if (prob == NULL) {
                            perror("Master problem realloc error");
                            exit(EXIT_FAILURE);
                        }
                        // 4. read the remaining data into the rest of the malloc'ed area
                        fwrite(prob->data, (prob->size - sizeof(struct problem)), 1, out);
                        debug("Write data");
                        // debug("M Got problem: size = %ld%s%d%s%d", prob->size, ", type = ", prob->type, ", variants = ", prob->nvars);
                        //ferror
                        if (ferror(out)) {
                            perror("ferror");
                            exit(EXIT_FAILURE);
                        }

                        debug("Flush");
                        if (fflush(out) == EOF) {
                            perror("fflush error");
                            exit(EXIT_FAILURE);
                        }

                        if (sigprocmask(SIG_SETMASK, &prev_all, NULL) < 0) { // unblock
                            perror("sigprocmask");
                            exit(EXIT_FAILURE);
                        }

                        sf_send_problem(worker_pid[w], prob);

                        // reads problem
                        // idle -> continued
                        debug("Sending SIGCONT to worker");

                        if (sigprocmask(SIG_BLOCK, &mask_all, &prev_all) < 0) { // block
                            perror("sigprocmask");
                            exit(EXIT_FAILURE);
                        }

                        kill(worker_pid[w], SIGCONT);
                        worker_states[get_pid_index(worker_pid[w])] = WORKER_CONTINUED;
                        sf_change_state(worker_pid[w], WORKER_IDLE, WORKER_CONTINUED);

                        if (sigprocmask(SIG_SETMASK, &prev_all, NULL) < 0) { // unblock
                            perror("sigprocmask");
                            exit(EXIT_FAILURE);
                        }
                    }

                    // posts results received from workers
                    // worker state = stopped
                    if (worker_states[w] == WORKER_STOPPED) { // STOPPED

                        if (sigprocmask(SIG_BLOCK, &mask_all, &prev_all) < 0) { // block
                            perror("sigprocmask");
                            exit(EXIT_FAILURE);
                        }

                        // READ RESULT
                        if ((in = fdopen(read_fd[w], "r")) == NULL) { // input
                            perror("Parent can't create input stream");
                            exit(EXIT_FAILURE);
                        }

                        // READ THE RESULT FROM CHILD
                        res = (struct result*) malloc(sizeof(struct result));
                        if (res == NULL) {
                            perror("Parent read result header malloc error");
                            exit(EXIT_FAILURE);
                        }
                        //debug("Read result");
                        //debug("Read data (nbytes = %ld%s", sizeof(struct result), ")");
                        // 1. read the header
                        fread(res, sizeof(struct result), 1, in); // ptr, size (each size bytes long), nmemb (items of data), stream
                        //ferror
                        if (ferror(in)) {
                            perror("ferror");
                            exit(EXIT_FAILURE);
                        }
                        // examine the size field of this structure and read an additional size
                        // 2. malloc storage for the full problem (header + data)
                        // 3. copy the already-read header into the beginning of the malloc'ed area
                        res = realloc(res, res->size); // realloc for new size (header + data)
                        if (res == NULL) {
                            perror("Parent read result realloc error");
                            exit(EXIT_FAILURE);
                        }
                        //debug("Got result: size = %ld%s%d", res->size, ", failed = ", res->failed);
                        // 4. read the remaining data into the rest of the malloc'ed area
                        fread(res->data, (res->size - sizeof(struct result)), 1, in);
                        //ferror
                        if (ferror(in)) {
                            perror("ferror");
                            exit(EXIT_FAILURE);
                        }

                        if (fflush(in) == EOF) {
                            perror("fflush error");
                            exit(EXIT_FAILURE);
                        }

                        if (sigprocmask(SIG_SETMASK, &prev_all, NULL) < 0) { // unblock
                            perror("sigprocmask");
                            exit(EXIT_FAILURE);
                        }

                        // master process has received a result over the pipe
                        sf_recv_result(worker_pid[w], res);

                        // CHECK RESULT
                        if(post_result(res, prob) == 0) { // if correct result
                            // cancel other workers that are doing this problem
                            // master process notify worker process to cancel solution procedure
                            for (int c = 0; c < workers; c++) {
                                if (worker_pid[c] != worker_pid[w]) { // skip this worker

                                    // and was solving actively solving a problem

                                    // cancel
                                    sf_cancel(worker_pid[c]);

                                    /*
                                    if (sigprocmask(SIG_BLOCK, &mask_all, &prev_all) < 0) { // block
                                        perror("sigprocmask");
                                        exit(EXIT_FAILURE);
                                    }
                                    */
                                    kill(worker_pid[c], SIGHUP);
                                    // sigsuspend(&mask_child);
                                    /*
                                    if (sigprocmask(SIG_SETMASK, &prev_all, NULL) < 0) { // unblock
                                        perror("sigprocmask");
                                        exit(EXIT_FAILURE);
                                    }
                                    */
                                }
                            }
                        }

                        free(res);

                        // change this worker state
                        // stopped -> idle
                        // kill(worker_pid[w], SIGSTOP);
                        if (sigprocmask(SIG_BLOCK, &mask_all, &prev_all) < 0) { // block
                            perror("sigprocmask");
                            exit(EXIT_FAILURE);
                        }
                        worker_states[get_pid_index(worker_pid[w])] = WORKER_IDLE;
                        sf_change_state(worker_pid[w], WORKER_STOPPED, WORKER_IDLE);
                        if (sigprocmask(SIG_SETMASK, &prev_all, NULL) < 0) { // unblock
                            perror("sigprocmask");
                            exit(EXIT_FAILURE);
                        }
                    }
                }
            }
        } else { // no more problems

            // until finally all of the worker processes have become idle and
            // a NULL return from the get_problem_variant function
            // indicates that there are no further problems to be solved
            // when all workers are IDLE
            // started -> idle -> running -> exited
            for (int w = 0; w < workers; w++) {
                // debug("%d%d", w, worker_states[w]);

                if (worker_states[w] == WORKER_STOPPED) {
                    // set to idle
                    if (sigprocmask(SIG_BLOCK, &mask_all, &prev_all) < 0) { // block
                        perror("sigprocmask");
                        exit(EXIT_FAILURE);
                    }
                    worker_states[get_pid_index(worker_pid[w])] = WORKER_IDLE;
                    sf_change_state(worker_pid[w], WORKER_STOPPED, WORKER_IDLE);
                    if (sigprocmask(SIG_SETMASK, &prev_all, NULL) < 0) { // unblock
                        perror("sigprocmask");
                        exit(EXIT_FAILURE);
                    }
                }

                // worker must be idle in order to terminate
                if (worker_states[w] == WORKER_IDLE || worker_states[w] == WORKER_RUNNING) {
                    // once this has occurred, a SIGTERM signal is sent to each of the worker processes
                    // which catch this signal and exit normally
                    // debug("no problems worker idle");

                    if (sigprocmask(SIG_BLOCK, &mask_all, &prev_all) < 0) { // block
                        perror("sigprocmask");
                        exit(EXIT_FAILURE);
                    }

                    kill(worker_pid[w], SIGTERM); // master sends SIGTERM
                    kill(worker_pid[w], SIGCONT); // sends SIGCONT
                    if (worker_states[w] != WORKER_EXITED) {
                        sigsuspend(&mask_child);
                    }

                    if (sigprocmask(SIG_SETMASK, &prev_all, NULL) < 0) { // unblock
                        perror("sigprocmask");
                        exit(EXIT_FAILURE);
                    }
                }
            }

            // When all of the worker processes have terminated, the master process itself
            // also terminates

            // exit status of the master process is EXIT_SUCCESS if all workers are EXIT_SUCCESS
            // otherwise exit status is EXIT_FAILURE

            if (fail == 1) {
                debug("EXIT_FAILURE");
                // free(res);
                sf_end();
                exit(EXIT_FAILURE);
            }

            if (done == workers && fail != 1) { // assumes all workers terminate
                debug("EXIT_SUCCESS");
                // close file descriptors
                for (int i = 0; i < workers; i++) {
                    if (read_fd[i] != 0 && read_fd[i] != 1 && read_fd[i] != 2 && write_fd[i] != 0 && write_fd[i] != 1 && write_fd[i] != 2) {
                        close(read_fd[i]);
                        close(write_fd[i]);
                    }
                }
                // free(res);
                sf_end();
                exit(EXIT_SUCCESS);
            }

        }
    } // main while loop

    return EXIT_FAILURE;
}
