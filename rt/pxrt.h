#ifndef PAXSY_PXRT_H
#define PAXSY_PXRT_H

/*
 * pxrt.h - the Paxsy runtime interface.
 * The C backend lowers channel operations onto the px_chan entry
 * points: a channel is an opaque handle carrying a bounded ring of
 * 64-bit slots guarded by one mutex and two condition variables. An
 * unbuffered channel (chan without a capacity) uses a one-slot ring;
 * senders block while the slot is occupied, receivers block while it
 * is empty, which gives rendezvous-like behaviour for the supported
 * programs. All channel entry points return an int status: 0 on
 * success, -1 after close (a receive on a closed, drained channel
 * yields the channel's zero value: a cleared slot).
 *
 * The paxsy_print_* entry points implement the std::inout::println
 * contract for the LLVM and MLIR emission paths, where println lowers
 * to a call of these shims instead of an emitted helper: the integer
 * form prints the value followed by a newline, the string form prints
 * the NUL-terminated byte string followed by a newline. Units
 * produced by --emit-llvm / --emit-mlir link against this runtime
 * together with the channel implementation.
 */

typedef struct px_chan px_chan;

/* Opens a channel with `cap` slots; cap 0 behaves as 1. */
px_chan *px_chan_new(long long cap);

/* Sends one 64-bit slot; blocks while the ring is full. Returns -1
 * when the channel is closed. */
int px_chan_send(px_chan *ch, long long v);

/* Receives one slot into *out; blocks while the ring is empty.
 * Returns -1 when the channel is closed and drained (the slot is
 * cleared to zero). */
int px_chan_recv(px_chan *ch, long long *out);

/* Closes the channel: waiting senders and future sends fail, waiting
 * receivers wake to drain the remaining slots and then see -1. */
void px_chan_close(px_chan *ch);

/* Releases the channel and its ring. */
void px_chan_free(px_chan *ch);

/* Prints one signed 32-bit integer followed by a newline; the
 * std::inout::println shim for the LLVM/MLIR emission paths. */
void paxsy_print_i32(int v);

/* Prints one NUL-terminated byte string followed by a newline; the
 * std::inout::println shim for the LLVM/MLIR emission paths. */
void paxsy_print_str(const char *s);

#endif /* PAXSY_PXRT_H */
