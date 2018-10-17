HPSC Mailbox Tester
===================

HPSC Mailbox Tester is a Linux application for sending a message and reading a
reply from a mailbox, either TRCH or RTPS. The tester follows a simple protocol
for request and reply messages, which is also followed by a command server
that processes requests on TRCH and/or RTPS.

Build
-----

Set the paths to the Poky SDK Aarch64 cross-compilation toolchain in Makefile and:

    make

Usage
-----

The tester writes a request to the output mailbox and reads a reply from
the input mailbox. The usage is:

    ./mboxtester [out_mbox_path|index in_mbox_path|index [cpu]]

The input and output mailboxes can be specified by either the full path to the
device file, e.g. `/sys/kernel/debug/mailbox/mbox0` or as an index which will
be expanded into a path, e.g. `0` will expand into the above path.

If no arguments are specified, the following default is assumed: `./mboxtester 0 1`.

The optional `cpu` argument allows to pin the execution of the tester to a core
with the given index.
