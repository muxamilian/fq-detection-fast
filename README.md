# Detect Fair Queuing on a connection (fast)

This repo has the exact same functionality as [fq-detection-simple](https://github.com/muxamilian/fq-detection-simple) but is faster since it's written in C while fq-detection-simple is written in Python. 
It's about twice as fast. It can detect fair queuing confidently on links which a capacity of up to around 3 Gbit/s. 

## Compiling 

Compile using gcc or clang like this: 

    gcc server.c -Ofast -o server && gcc client.c -Ofast -o client

Then run as described in the readme of [fq-detection-simple](https://github.com/muxamilian/fq-detection-simple). 
