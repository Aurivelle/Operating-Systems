# NTU Operating Systems Assignments (CSIE3310)

This repository contains the implementation of four machine problems (MP0 ~ MP4) OS course, Spring 2025. Each assignment focuses on different core concepts in operating systems, including system calls, threading, scheduling, memory management, and file systems.


## Overview

### MP0 - Hello xv6

- Setup xv6 and Docker environment
- Write a user-level program and a syscall to print "Hello World"

### MP1 - User-level Thread Library

- Implement cooperative user-level threads
- Support basic thread creation and scheduling

### MP2 - SLAB Allocator

- Implement SLAB memory allocator with freelists
- Manage full/partial/free slab classification

### MP3 - Thread Scheduling

- **Non-Real-Time:**
  - HRRN (Highest Response Ratio Next)
  - Priority-based Round Robin
- **Real-Time:**
  - Deadline Monotonic (DM)
  - EDF with Constant Bandwidth Server (EDF-CBS)

### MP4 - File System

- **Problem 1: Access Control & Symbolic Links**
  - Implement `chmod`, `symln`, and permission-aware `ls`
  - Extend `open()` syscall with `O_NOACCESS`
- **Problem 2: RAID 1 Simulation**
  - Implement mirrored writes and fallback logic for disk failures
  - Add diagnostic `printf()` traces
