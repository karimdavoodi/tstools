/*
 * Given an M2TS random access transport stream (BDAV MPEG-2 TS),
 * reorder the packets and strip off the time codes to give a normal
 * TS.
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is the MPEG TS, PS and ES tools.
 *
 * The Initial Developer of the Original Code is Amino Communications Ltd.
 * Portions created by the Initial Developer are Copyright (C) 2008
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Kynesim Ltd, Cambridge UK
 *
 * ***** END LICENSE BLOCK *****
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#ifdef _WIN32
#include <stddef.h>
#else // _WIN32
#include <unistd.h>
#endif // _WIN32

#include "compat.h"
#include "ts_defns.h"
#include "misc_fns.h"
#include "version.h"

#define M2TS_PACKET_SIZE (4 + TS_PACKET_SIZE)

struct _m2ts_packet_buffer
{
  struct _m2ts_packet_buffer * next;
  struct _m2ts_packet_buffer * prev;
  uint32_t timestamp;
  byte   * ts_packet;
  byte     m2ts_packet[M2TS_PACKET_SIZE];
};

typedef struct _m2ts_packet_buffer *m2ts_packet_buffer_p;



/*
 * Extract the timestamp from an M2TS packet and set up
 * the internal data structure pointers.
 */

static void parse_m2ts_packet(m2ts_packet_buffer_p packet_buffer)
{
  packet_buffer->timestamp =
    (((uint32_t)(packet_buffer->m2ts_packet[0])) << 24) |
    (((uint32_t)(packet_buffer->m2ts_packet[1])) << 16) |
    (((uint32_t)(packet_buffer->m2ts_packet[2])) << 8) |
    ((uint32_t)(packet_buffer->m2ts_packet[3]));
  packet_buffer->ts_packet = packet_buffer->m2ts_packet + 4;
  packet_buffer->next = NULL;
  packet_buffer->prev = NULL;
}



/*
 * Read in M2TS packets, strip the timestamp, sort by timestamp and
 * write out TS packets.
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */

static int extract_packets(int input, FILE * output,
			   const unsigned int reorder_buffer_size,
			   int verbose, int quiet)
{
  int err;
  m2ts_packet_buffer_p reorder_buffer_head = NULL;
  m2ts_packet_buffer_p reorder_buffer_tail = NULL;
  int reorder_buffer_entries = 0;
  m2ts_packet_buffer_p packet_buffer_in_hand = NULL;
  m2ts_packet_buffer_p packet_buffer;
  m2ts_packet_buffer_p p;
  int written;
  
  // For test purposes, just grab packets and print the time stamps
  while (1)
  {
    // Get a new packet buffer
    if (packet_buffer_in_hand != NULL)
    {
      packet_buffer = packet_buffer_in_hand;
      packet_buffer_in_hand = NULL;
    }
    else
    {
      packet_buffer = malloc(sizeof(struct _m2ts_packet_buffer));
      /***DEBUG***/
      printf("Allocated buffer @ 0x%08x\n", (unsigned int)packet_buffer);
      if (packet_buffer == NULL)
      {
	KLOG( "### m2ts2ts: out of memory allocating M2TS packet buffer\n");
	while (reorder_buffer_head != NULL)
	{
	  packet_buffer = reorder_buffer_head->next;
	  free(reorder_buffer_head);
	  reorder_buffer_head = packet_buffer;
	}
	if (packet_buffer_in_hand != NULL)
	  free(packet_buffer_in_hand);
	return 1;
      }
    }
    err = read_bytes(input, M2TS_PACKET_SIZE, packet_buffer->m2ts_packet);
    if (err == EOF)
    {
      // End of file, no more to do, thank you and goodnight
      if (!quiet)
	printf("m2ts2ts: Reached end of file\n");
      break;
    }
    else if (err)
    {
      // Badness has occurred, no point in saying more here
      while (reorder_buffer_head != NULL)
      {
	packet_buffer = reorder_buffer_head->next;
	free(reorder_buffer_head);
	reorder_buffer_head = packet_buffer;
      }
      if (packet_buffer_in_hand != NULL)
	free(packet_buffer_in_hand);
      return 1;
    }
    parse_m2ts_packet(packet_buffer);
    if (verbose)
      printf("Read timestamp 0x%08x\n", packet_buffer->timestamp);

    // Insert the packet in the reorder buffer, in time order
    // It's most likely that we'll get an up to date packet,
    // so start at the tail and work to the front
    p = reorder_buffer_tail;
    if (p != NULL)
      printf("tail timestamp = 0x%08x @ 0x%08x\n",
	     p->timestamp, (unsigned int)p);
    while (p != NULL && p->timestamp > packet_buffer->timestamp)
    {
      p = p->prev;
      if (p != NULL)
	printf("p timestamp = 0x%08x @ 0x%08x\n",
	       p->timestamp, (unsigned int)p);
    }

    if (p == NULL)
    {
      // Insert as the head of queue
      printf ("### Insert 0x%08x at head: 0x%08x\n",
	      (unsigned int)packet_buffer,
	      (unsigned int)reorder_buffer_head);
      packet_buffer->next = reorder_buffer_head;
      reorder_buffer_head = packet_buffer;
      packet_buffer->prev = NULL;
      if (reorder_buffer_tail == NULL)
      {
	// I.e. this is the only entry on the queue
	reorder_buffer_tail = packet_buffer;
      }
      else
      {
	packet_buffer->next->prev = packet_buffer;
      }
    }
    else
    {
      // At this point, p points to the previous packet to our new one
      packet_buffer->next = p->next;
      p->next = packet_buffer;
      packet_buffer->prev = p;
      if (p == reorder_buffer_tail)
      {
	// I.e. we have inserted at the end
	reorder_buffer_tail = packet_buffer;
      }
      else
      {
	if (verbose)
	  printf("Reordered packet timestamp=0x%08x\n",
		 packet_buffer->timestamp);
	packet_buffer->next->prev = packet_buffer;
      }
    }
    printf("### packet at 0x%08x, prev=0x%08x, next=0x%08x\n",
	   (unsigned int)packet_buffer,
	   (unsigned int)(packet_buffer->prev),
	   (unsigned int)(packet_buffer->next));
    reorder_buffer_entries++;

    if (reorder_buffer_entries > (int)reorder_buffer_size)
    {
      // Write out the head of the reorder buffer
      printf("### queue head @ 0x%08x, next=0x%08x\n",
	     (unsigned int)reorder_buffer_head,
	     (unsigned int)(reorder_buffer_head->next));
      packet_buffer = reorder_buffer_head;
      reorder_buffer_head = reorder_buffer_head->next;
      reorder_buffer_head->prev = NULL;
      written = fwrite(packet_buffer->ts_packet,
		       TS_PACKET_SIZE, 1, output);
      if (written != 1)
      {
	// Major output catastrophe!
	KLOG( "### m2ts2ts: Error writing TS packet: %s\n",
		strerror(errno));
	free(packet_buffer);
	while (reorder_buffer_head != NULL)
	{
	  packet_buffer = reorder_buffer_head->next;
	  free(reorder_buffer_head);
	  reorder_buffer_head = packet_buffer;
	}
	// No packet in hand here
	return 1;
      }

      reorder_buffer_entries--;
      if (verbose)
	printf("Written timestamp 0x%08x\n", packet_buffer->timestamp);
      packet_buffer_in_hand = packet_buffer;
    }
  }

  free(packet_buffer_in_hand);

  // Write out the remaining packets in the reorder buffer
  while (reorder_buffer_head != NULL)
  {
    packet_buffer = reorder_buffer_head->next;
    written = fwrite(reorder_buffer_head->ts_packet,
		     TS_PACKET_SIZE, 1, output);
    if (written != 1)
    {
      // So close...
      KLOG( "### m2ts2ts: Error writing final TS packets: %s\n",
	      strerror(errno));
      while (reorder_buffer_head != NULL)
      {
	packet_buffer = reorder_buffer_head->next;
	free(reorder_buffer_head);
	reorder_buffer_head = packet_buffer;
      }
    }
    free(reorder_buffer_head);
    reorder_buffer_head = packet_buffer;
  }

  return 0;
}



static void print_usage(void)
{
  printf("Usage: m2ts2es [switches] [<infile>] [<outfile>]\n"
	 "\n");
  REPORT_VERSION("m2ts2ts");
  printf("\n"
	 "Files:\n"
	 "  <infile>  is a BDAV MPEG-2 Transport Stream file (M2TS)\n"
	 "            (but see -stdin)\n"
	 "  <outfile> is an H.222 Transport Stream file (but see -stdout)\n"
	 "\n"
	 "General Switches:\n"
	 "  -stdin               Input from standard input instead of a file\n"
	 "  -stdout              Output to standard output instead of a file\n"
	 "  -verbose, -v         Output informational/diagnostic messages\n"
	 "  -quiet, -q           Only output error messages\n"
	 "  -buffer <n>, -b <n>  Number of TS packets to buffer for reordering\n"
	 "                       Defaults to 4.\n");
}
	 


int main(int argc, char *argv[])
{
  int   use_stdout = FALSE;
  int   use_stdin = FALSE;
  char *input_name = NULL;
  char *output_name = NULL;
  int   had_input_name = FALSE;
  int   had_output_name = FALSE;

  int   input            = -1;    // Our input file descriptor
  FILE *output           = NULL;  // Our output stream (if any)
  unsigned int reorder_buff_size = 4; // Number of TS packets to delay output
  int   quiet            = FALSE; // True => be as quiet as possible
  int   verbose          = FALSE; // True => output diagnostic messages

  int err = 0;
  int ii = 1;

  if (argc < 2)
  {
    print_usage();
    return 0;
  }

  // Extract parameters
  while (ii < argc)
  {
    if (argv[ii][0] == '-')
    {
      if (!strcmp("--help", argv[ii]) || !strcmp("-h", argv[ii]) ||
	  !strcmp("-help", argv[ii]))
      {
	print_usage();
	return 0;
      }
      else if (!strcmp("-verbose", argv[ii]) || !strcmp("-v", argv[ii]))
      {
	verbose = TRUE;
	quiet = FALSE;
      }
      else if (!strcmp("-quiet", argv[ii]) || !strcmp("-q", argv[ii]))
      {
	verbose = FALSE;
	quiet = TRUE;
      }
      else if (!strcmp("-buffer", argv[ii]) || !strcmp("-b", argv[ii]))
      {
	err = unsigned_value("ts2es", argv[ii], argv[ii+1],
			     0, &reorder_buff_size);
	if (err) return 1;
	ii++;
      }
      else if (!strcmp("-stdin", argv[ii]))
      {
	use_stdin = TRUE;
	had_input_name = TRUE; // and it's "stdin"...
      }
      else if (!strcmp("-stdout", argv[ii]))
      {
	use_stdout = TRUE;
	had_output_name = TRUE; // ish
      }
      else
      {
	KLOG( "### m2ts2ts: "
		"Unrecognised command line switch '%s'\n", argv[ii]);
	return 1;
      }
    }
    else
    {
      if (had_input_name && had_output_name)
      {
	KLOG( "### m2ts2ts: Unexpected '%s'\n", argv[ii]);
	return 1;
      }
      else if (had_input_name) // and not had_output_name, inc "-stdout"
      {
	output_name = argv[ii];
	had_output_name = TRUE;
      }
      else // had_output_name && !had_input_name
      {
	input_name = argv[ii];
	had_input_name = TRUE;
      }
    }
    ii++;
  }

  if (!had_input_name)
  {
    KLOG( "### m2ts2ts: No input file specified\n");
    return 1;
  }

  if (!had_output_name)
  {
    KLOG( "### m2ts2ts: No output file specified\n");
    return 1;
  }

  // Stop (as far as possible) extraneous data ending up in our output stream
  if (use_stdout)
  {
    verbose = FALSE;
    quiet = TRUE;
  }

  if (use_stdin)
  {
    input = STDIN_FILENO;
  }
  else
  {
    input = open_binary_file(input_name, FALSE);
    if (input == -1)
    {
      KLOG( "### m2ts2ts: Unable to open input file %s\n",
	      input_name);
      return 1;
    }
  }
  if (!quiet)
    printf("Reading from %s\n", (use_stdin ? "<stdin>" : input_name));

  if (use_stdout)
  {
    output = stdout;
  }
  else
  {
    output = fopen(output_name, "wb");
    if (output == NULL)
    {
      if (!use_stdin)
	(void) close_file(input);
      KLOG( "### m2ts2ts: Unable to open output file %s: %s\n",
	      output_name, strerror(errno));
      return 1;
    }
  }
  if (!quiet)
    printf("Writing to   %s\n", (use_stdout ? "<stdout>" : output_name));


  err = extract_packets(input, output, reorder_buff_size, verbose, quiet);
  if (err)
  {
    KLOG( "### m2ts2ts: Error extracting data\n");
    if (!use_stdin)  (void) close_file(input);
    if (!use_stdout) (void) fclose(output);
    return 1;
  }

  // Now tidy up
  if (!use_stdout)
  {
    errno = 0;
    err = fclose(output);
    if (err)
    {
      KLOG( "### m2ts2ts: Error closing output file %s: %s\n",
	      output_name, strerror(errno));
      (void) close_file(input);
      return 1;
    }
  }

  if (!use_stdin)
  {
    err = close_file(input);
    if (err)
      KLOG( "### m2ts2ts: Error closing input file %s\n",
	      input_name);
  }
  return 0;
}


// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:
