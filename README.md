# User-Level Threads Library (uthreads)
This repository contains the implementation of a User-Level Threads Library (uthreads) for managing user-level threads in a program. The library provides functions to create, schedule, and terminate threads, allowing for concurrent execution of multiple threads within a single process.

## Features
- Lightweight user-level threads: Create and manage multiple threads within a program.
- Round-Robin scheduling: Utilize the Round-Robin (RR) scheduling algorithm to fairly allocate CPU time to each thread.
- Blocking and resuming threads: Block and resume threads based on specific conditions or events.
- Sleeping threads: Put threads to sleep for a specified number of quantums.
- Thread statistics: Retrieve information about thread IDs, total quantums, and individual thread quantums.
  
## Getting Started
To get started with using the User-Level Threads Library, follow these steps:

1. Clone the repository:
   ``` git clone https://github.com/IsraelBenDavid/User-Level-Threads.git ```
2. Include the uthreads.h header file in your project.
3. Build the library using the provided Makefile: make
4. Link against the generated static library libuthreads.a.
5. Use the library functions to create and manage threads in your program.

## API Documentation
The library provides the following functions:

- int uthread_init(int quantum_usecs): Initialize the thread library with the specified quantum in microseconds.
- int uthread_spawn(thread_entry_point entry_point): Create a new thread with the given entry point.
- int uthread_terminate(int tid): Terminate the thread with the specified ID.
- int uthread_block(int tid): Block the thread with the specified ID.
- int uthread_resume(int tid): Resume a blocked thread with the specified ID.
- int uthread_sleep(int num_quantums): Block the running thread for a specified number of quantums.
- int uthread_get_tid(): Get the ID of the calling thread.
- int uthread_get_total_quantums(): Get the total number of quantums since library initialization.
- int uthread_get_quantums(int tid): Get the number of quantums the thread with the specified ID has run.

Please refer to the uthreads.h header file for detailed function descriptions and usage guidelines.

## Examples
To see examples of how to use the User-Level Threads Library, please refer to the examples directory in this repository. The examples demonstrate various threading scenarios and showcase the library's features.

## Contributing
Contributions to the User-Level Threads Library project are welcome! If you find any issues or have suggestions for improvements, please open an issue or submit a pull request.

## Acknowledgments
This project was developed as part of the OS course at the Hebrew University. Special thanks to the course instructors for their guidance and support.
