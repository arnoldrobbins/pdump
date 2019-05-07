# pdump --- A Program To Read Pr1me MAGSAVE Tapes

This program reads tapes in MAGSAVE format created by Pr1me computers.

## History

The program was written in the summer of 1985 by Scott Lee (then known as Jeff Lee). Scott was a member of the lab staff at the Georgia Tech School of Information and Computer Science (ICS).

It was written to enable moving users' files off of the school's three Pr1me computers and onto the relatively new DEC VAX-11/780 running 4.1 BSD Unix.

Arnold Robbins, who also worked at the ICS Lab, kept a copy. It's being revived now in the hope that it will eventually be useful in recovering the source code for the Georgia Tech *Software Tools Subsystem for Prime Comnputers*. 

An attempt has been made to preserve the original modification dates as the commit dates to the Git repository.

## Contents

It's pretty straightforward:

`pdump.c`: The program.
`pdump.1`: The man page.
`Makefile`: The makefile for `make`.
`ChangeLog`: A GNU-style ChangeLog file to track changes going forward.
`README.md`: This filie.

## Future work

1. Convert the code to read from a file instead of requiring a tape device.

1. Convert the code to ISO C, either C89 or C99.

1. Use modern system calls, such as *mkdir*(2) instead of forking `/bin/mkdir`.

1. Anything else that makes sense to make the code meet current coding styles and conventions.

#### Last Updated

Tue May  7 21:37:27 IDT 2019
~~~~
