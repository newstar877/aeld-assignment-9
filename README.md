# aesd-assignment-9
This repo contains public starter source code, scripts, and documentation for Advanced Embedded Software Development (ECEN-5713) and Advanced Embedded Linux Development assignments University of Colorado, Boulder. In addition it contains the completed code for assignment-9, the kernel char driver and related socket application.

## Setting Up Git

Use the instructions at [Setup Git](https://help.github.com/en/articles/set-up-git) to perform initial git setup steps. For AESD you will want to perform these steps inside your Linux host virtual or physical machine, since this is where you will be doing your development work.

## Setting up SSH keys

See instructions in [Setting-up-SSH-Access-To-your-Repo](https://github.com/cu-ecen-aeld/aesd-assignments/wiki/Setting-up-SSH-Access-To-your-Repo) for details.

## Specific Assignment Instructions

The specific tasks for this assignment can be found in the end of project exercise, Assignment 9 Instructions, after Module 4 of the Coursera course.

## Testing

The following are the test results from the two test scripts that check the driver & socket app implementation respectively:

drivertest:
```text
aesdchar_load
Local file aesdchar.ko not found, attempting to modprobe
[   71.364967] aesdchar: loading out-of-tree module taints kernel.
root@qemuarm64:/usr/bin/assignment-autotest/test/assignment9-yocto# aesdsocket -d
root@qemuarm64:/usr/bin/assignment-autotest/test/assignment9-yocto# ./drivertest.sh 
The output below should show write 1 with first 2 bytes missing
ite1
write2
write3
write4
write5
write6
write7
write8
write9
write10
The output below should show the 9 from write 9 followed by write10 only
9
write10
```
sockettest:
```text
./sockettest.sh 
Testing target localhost on port 9000
Sending ioc seekto command for offset 0,2
rite1
swrite2
swrite3
swrite4
swrite5
swrite6
swrite7
swrite8
swrite9
swrite10
Sending ioc seekto command for offset 8,6
9
swrite10
Test passed
```


