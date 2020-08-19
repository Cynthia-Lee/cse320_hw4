#include <stdlib.h>

#include "debug.h"
#include "polya.h"

volatile sig_atomic_t canceledp = 0;
volatile sig_atomic_t done = 0;

// SIGHUP handler
// signal sent by master process to notify a worker to cancel its current solution attempt
void sighup_handler(int sig) {
    // worker process receives SIGHUP
    // if current solution attempt has not succeeded or failed
    // then it is abandoned and a result marked "failed" is sent to the master process
    // before the worker stops by sending itself a SIGSTOP signal
    canceledp = 1;
    debug("SIGHUP received -- canceling current solution attempt");
}

// SIGTERM handler
// signal sent by the master to cause graceful termination of a worker process
void sigterm_handler(int sig) {
    // it will abandon any current solution attempt
    // use exit(3) to terminate normally with status EXIT_SUCCESS
    done = 1;
    debug("SIGTERM received");
    exit(EXIT_SUCCESS);
}

/*
 * worker
 * (See polya.h for specification.)
 */
int worker(void) {
    // TO BE IMPLEMENTED

    // INITIALIZATION
    // during the creation of each worker process, the master process creates the
        // associated pipes and performs I/O redirection so that the
        // worker always reads problems from its standard input (file descriptor 0)
        // and writes results to its standard output (file descriptor 1)

    // redirects its standard input and standard output to the pipes using dup2(2) system call
    // the worker program itself is executing using one of the system calls in the exec(3) family
    // fdopen(3) to wrap the pipe file descriptors
    // actual reading and writing of data on  pipes (using standard I/O library)

    debug("Starting");
    done = 0;
    sigset_t sighup_mask;
    sigset_t orig_mask;
    sigemptyset(&sighup_mask);
    sigaddset(&sighup_mask, SIGHUP);

    if (signal(SIGTERM, sigterm_handler) == SIG_ERR) { // Install the handler
        perror("signal_error");
        exit(EXIT_FAILURE);
    }
    if (signal(SIGHUP, sighup_handler) == SIG_ERR) { // Install the handler
        perror("signal_error");
        exit(EXIT_FAILURE);
    }

    // read stdin(fd = 0) and write stdout(fd = 1)

    // performs required initilization, then stops by sending itself a SIGSTOP signal
    // idle
    debug("Idling (sending SIGSTOP to self)");
    if (raise(SIGSTOP) != 0) {
        perror("Child could not send SIGSTOP to itself");
        exit(EXIT_FAILURE);
    }

    // upon continuing (when the master process sends a SIGCONT signal)
        // it reads a problem sent by the master and attempts to solve the problem
    // continued state

    // READING
    // worker process wants to read a problem sent by the master
    // first reads the fixed-size problem header, and continues reading the problem data
    // problem data - the number of bytes can be calculated by
        // subtracting the size of the header from the total size given in the size field of the header
    // worker sends a result to the master is symmetric

    while (done == 0) {
        // canceledp = 0;
        // read sizeof(struct problem) bytes from the input and store it into a struct problem variable
        // malloc to get header
        struct problem *m_problem = (struct problem*) malloc(sizeof(struct problem));
        if (m_problem == NULL) {
            perror("Child problem header malloc error");
            exit(EXIT_FAILURE);
        }
        debug("Reading problem");
        debug("Reading data (nbytes = %ld%s", sizeof(struct problem), ")");
        // 1. read the header
        fread(m_problem, sizeof(struct problem), 1, stdin); // ptr, size (each size bytes long), nmemb (items of data), stream
        //ferror
        if (ferror(stdin)) {
            perror("ferror");
            exit(EXIT_FAILURE);
        }
        // examine the size field of this structure and read an additional size
        // 2. malloc storage for the full problem (header + data)
        // 3. copy the already-read header into the beginning of the malloc'ed area
        m_problem = realloc(m_problem, m_problem->size); // realloc for new size (header + data)
        if (m_problem == NULL) {
            perror("Child problem realloc error");
            exit(EXIT_FAILURE);
        }
        debug("Got problem: size = %ld%s%d%s%d", m_problem->size, ", type = ", m_problem->type, ", variants = ", m_problem->nvars);
        // 4. read the remaining data into the rest of the malloc'ed area
        fread(m_problem->data, (m_problem->size - sizeof(struct problem)), 1, stdin);
        //ferror
        if (ferror(stdin)) {
            perror("ferror");
            exit(EXIT_FAILURE);
        }
        if (fflush(stdin) == EOF) {
            perror("fflush error");
            exit(EXIT_FAILURE);
        }
        debug("Solving problem");
        // SOLVING
        if (sigprocmask(SIG_BLOCK, &sighup_mask, &orig_mask) < 0) { // block
            perror("sigprocmask");
            exit(EXIT_FAILURE);
        }
        volatile sig_atomic_t *canceledp_ptr = &canceledp;
        struct result *solver = (struct result *)(solvers[m_problem->type].solve(m_problem, canceledp_ptr));
        if (sigprocmask(SIG_SETMASK, &orig_mask, NULL) < 0) { // unblock
            perror("sigprocmask");
            exit(EXIT_FAILURE);
        }
        if (canceledp == 1) {
            solver->failed = 1;
            canceledp = 0;
        }
        //debug("%ld", solver->size);
        //debug("%d", solver->id);
        //debug("%d", solver->failed); // 0 if result not "failed", nonzero if result "failed"
        //debug("%s", solver->data);

        // continues the solution attempt until it either succeeds in finding a solution,
            // fails to find a solution, or is notified (by the master sending SIGHUP to cancel)
            // sends a result to the master

        // 1) A solution is found
        // 2) the solution procedure fails
        // 3) (SIGHUP) the master process notifies the worker ot cancel the solution procedure

        // WRITING A RESULT
        fwrite(solver, solver->size, 1, stdout);
        // ferror
        if (ferror(stdout)) {
            perror("ferror");
            exit(EXIT_FAILURE);
        }
        if (fflush(stdout) == EOF) {
            perror("fflush error");
            exit(EXIT_FAILURE);
        }
        free(m_problem);
        free(solver);

        // send result to the master process
        debug("Raised SIGSTOP");
        if (raise(SIGSTOP) != 0) {
            perror("Child could not send SIGSTOP to itself");
            exit(EXIT_FAILURE);
        }
    }

    return EXIT_FAILURE;
}
