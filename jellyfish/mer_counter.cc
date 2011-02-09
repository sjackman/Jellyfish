/*  This file is part of Jellyfish.

    Jellyfish is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Jellyfish is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Jellyfish.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <config.h>
#include <pthread.h>
#include <fstream>
#include <exception>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <argp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>
#include <inttypes.h>

#include <jellyfish/misc.hpp>
#include <jellyfish/time.hpp>
#include <jellyfish/mer_counting.hpp>
#include <jellyfish/locks_pthread.hpp>
#include <jellyfish/fasta_parser.hpp>
#include <jellyfish/thread_exec.hpp>
#include <jellyfish/square_binary_matrix.hpp>

/*
 * Option parsing
 */
static char doc[] = "Count k-mers in a fast[aq] files";
static char args_doc[] = "fasta|fastq ...";
     
enum {
  OPT_VAL_LEN = 1024,
  OPT_BUF_SIZE,
  OPT_TIMING,
  OPT_OBUF_SIZE,
  OPT_MATRIX,
  OPT_QUAL_CON
};
static struct argp_option options[] = {
  {"threads",           't',            "NB",   0,              "Nb of threads", 1},
  {"mer-len",           'm',            "LEN",  0,              "Length of a mer", 0},
  {"counter-len",       'c',            "LEN",  0,              "Length (in bits) of counting field"},
  {"output",            'o',            "FILE", 0,              "Output file", 1},
  {"out-counter-len",   OPT_VAL_LEN,    "LEN",  0,              "Length (in bytes) of counting field in output", 1},
  {"buffers",           'b',            "NB",   OPTION_HIDDEN,  "Nb of buffers per thread"},
  {"fastq",             'q',            0,      0,              "Fastq input files", 2},
  {"quality-control",   OPT_QUAL_CON,   0,      0,              "B quality in fastq is Read Segment Quality Control Indicator", 2},
  {"both-strands",      'C',            0,      0,              "Count both strands, canonical representation", 1},
  {"buffer-size",       OPT_BUF_SIZE,   "SIZE", OPTION_HIDDEN,  "Size of a buffer"},
  {"out-buffer-size",   OPT_OBUF_SIZE,  "SIZE", 0,              "Size of output buffer per thread"},
  {"hash-size",         's',            "SIZE", 0,              "Hash size", 0},
  {"size",              's',            "SIZE", OPTION_ALIAS,   "Hash size"},
  {"reprobes",          'p',            "NB",   0,              "Maximum number of reprobing", 1},
  {"no-write",          'w',            0,      0,              "Don't write hash to disk", 3},
  {"raw",               'r',            0,      0,              "Dump raw database", 2},
  {"matrix",            OPT_MATRIX,     "FILE", 0,              "Matrix for hash function", 3},
  {"timing",            OPT_TIMING,     "FILE", 0,              "Print timing information to FILE", 3},
  { 0 }
};

struct arguments {
  unsigned long	 nb_threads;
  unsigned long	 buffer_size;
  unsigned long	 nb_buffers;
  unsigned long	 mer_len;
  unsigned long	 counter_len;
  unsigned long	 out_counter_len;
  unsigned long  reprobes;
  unsigned long	 size;
  unsigned long	 out_buffer_size;
  bool           no_write;
  bool           both_strands;
  bool           raw;
  char          *timing;
  char          *output;
  char          *matrix;
  bool           fastq;
  bool           quality_control;
};

static error_t parse_opt (int key, char *arg, struct argp_state *state)
{
  struct arguments *arguments = (struct arguments *)state->input;
  error_t error;

#define ULONGP(field) \
  error = parse_long(arg, &std::cerr, &arguments->field);       \
  if(error) return(error); else break;

#define FLAG(field) arguments->field = true; break;

#define STRING(field) arguments->field = arg; break;

  switch(key) {
  case 't': ULONGP(nb_threads);
  case 's': ULONGP(size); 
  case 'b': ULONGP(nb_buffers); 
  case 'm': ULONGP(mer_len); 
  case 'c': ULONGP(counter_len); 
  case 'p': ULONGP(reprobes); 
  case 'w': FLAG(no_write);
  case 'r': FLAG(raw);
  case 'q': FLAG(fastq);
  case 'C': FLAG(both_strands);
  case 'o': STRING(output);
  case OPT_VAL_LEN: ULONGP(out_counter_len); 
  case OPT_BUF_SIZE: ULONGP(buffer_size); 
  case OPT_TIMING: STRING(timing);
  case OPT_OBUF_SIZE: ULONGP(out_buffer_size); 
  case OPT_MATRIX: STRING(matrix);
  case OPT_QUAL_CON: FLAG(quality_control);
    
  default:
    return ARGP_ERR_UNKNOWN;
  }
  return 0;
}
static struct argp argp = { options, parse_opt, args_doc, doc };

class mer_counting_base {
public:
  virtual void count() = 0;
  virtual Time get_writing_time() = 0;
  virtual ~mer_counting_base() {}
};

template <typename parser_t, typename hash_t>
class mer_counting : public mer_counting_base, public thread_exec {
protected:
  struct arguments            arguments;
  locks::pthread::barrier     sync_barrier;
  parser_t                   *parser;
  typename hash_t::storage_t *ary;
  hash_t                     *hash;
  jellyfish::dumper_t        *dumper;

public:
  mer_counting(struct arguments &args) :
    arguments(args), sync_barrier(arguments.nb_threads) {}

  ~mer_counting() { 
    if(dumper)
      delete dumper;
    if(hash)
      delete hash;
    if(ary)
      delete ary;
    if(parser)
      delete parser;
  }
  
  void start(int id) {
    sync_barrier.wait();
    
    typename parser_t::thread     mer_stream(parser->new_thread());
    typename hash_t::thread_ptr_t counter(hash->new_thread());
    mer_stream.parse(counter);
    
    bool is_serial = sync_barrier.wait() == PTHREAD_BARRIER_SERIAL_THREAD;
    if(is_serial)
      hash->dump();
  }
  
  void count() {
    exec_join(arguments.nb_threads);
  }

  Time get_writing_time() { return hash->get_writing_time(); }
};


class mer_counting_fasta_hash : public mer_counting<jellyfish::fasta_parser, inv_hash_t> {
public:
  mer_counting_fasta_hash(int arg_st, int argc, char *argv[], struct arguments &_args) :
    mer_counting<jellyfish::fasta_parser, inv_hash_t>(_args)
  {
    parser = new jellyfish::fasta_parser(argc - arg_st, argv + arg_st, 
                                         arguments.mer_len, arguments.nb_buffers,
                                         arguments.buffer_size);
    ary = new inv_hash_t::storage_t(arguments.size, 2*arguments.mer_len,
                                    arguments.counter_len, 
                                    arguments.reprobes, 
                                    jellyfish::quadratic_reprobes);
    if(_args.matrix) {
      std::ifstream fd;
      fd.exceptions(std::ifstream::eofbit|std::ifstream::failbit|std::ifstream::badbit);
      fd.open(_args.matrix);
      SquareBinaryMatrix m(&fd);
      fd.close();
      ary->set_matrix(m);
    }
    hash = new inv_hash_t(ary);

    if(!arguments.no_write) {
      if(arguments.raw) {
        dumper = new raw_inv_hash_dumper_t((uint_t)4, arguments.output, arguments.out_buffer_size, ary);
      } else {
        dumper = new inv_hash_dumper_t(arguments.nb_threads, arguments.output,
                                       arguments.out_buffer_size,
                                       8*arguments.out_counter_len,
                                       ary);
      }
      hash->set_dumper(dumper);
    }
    parser->set_canonical(arguments.both_strands);
  }
};

class mer_counting_fasta_direct : public mer_counting<jellyfish::fasta_parser, direct_index_t> {
public:
  mer_counting_fasta_direct(int arg_st, int argc, char *argv[], struct arguments &_args) :
    mer_counting<jellyfish::fasta_parser, direct_index_t>(_args)
  {
    parser = new jellyfish::fasta_parser(argc - arg_st, argv + arg_st, 
                                         arguments.mer_len, arguments.nb_buffers,
                                         arguments.buffer_size);
    ary = new direct_index_t::storage_t(2 * arguments.mer_len);
    hash = new direct_index_t(ary);
    if(!arguments.no_write) {
      if(arguments.raw)
        std::cerr << "Switch --raw not (yet) supported with direct indexing. Ignoring." << std::endl;
      dumper = new direct_index_dumper_t(arguments.nb_threads, arguments.output,
                                         arguments.out_buffer_size,
                                         8*arguments.out_counter_len,
                                         ary);
      hash->set_dumper(dumper);
    }
    parser->set_canonical(arguments.both_strands);
  }
};

class mer_counting_fastq : public mer_counting<jellyfish::fastq_parser, fastq_hash_t> {
public:
  mer_counting_fastq(int arg_st, int argc, char *argv[], struct arguments &_args) :
    mer_counting<jellyfish::fastq_parser, fastq_hash_t>(_args)
  {
    parser = new jellyfish::fastq_parser(argc - arg_st, argv + arg_st,
                                         arguments.mer_len, arguments.nb_buffers, 
                                         arguments.quality_control);
    ary = new fastq_hash_t::storage_t(arguments.size, 2*arguments.mer_len,
                                      arguments.reprobes, 
                                      jellyfish::quadratic_reprobes);
    hash = new fastq_hash_t(ary);
    if(!arguments.no_write) {
      dumper = new raw_fastq_dumper_t(arguments.nb_threads, arguments.output,
                                      arguments.out_buffer_size,
                                      ary);
      hash->set_dumper(dumper);
    }
    parser->set_canonical(arguments.both_strands);
  }
};

int count_main(int argc, char *argv[]) {
  struct arguments arguments;
  int arg_st;

  arguments.nb_threads      = 1;
  arguments.mer_len         = 0;
  arguments.counter_len     = 7;
  arguments.out_counter_len = 4;
  arguments.size            = 0;
  arguments.reprobes        = 50;
  arguments.nb_buffers      = 100;
  arguments.buffer_size     = 4096;
  arguments.out_buffer_size = 20000000UL;
  arguments.no_write        = false;
  arguments.raw             = false;
  arguments.fastq           = false;
  arguments.both_strands    = false;
  arguments.timing          = NULL;
  arguments.matrix          = NULL;
  arguments.output          = (char *)"mer_counts.hash";
  int error = argp_parse(&argp, argc, argv, 0, &arg_st, &arguments);
  if(error) {
    fprintf(stderr, "Error parsing arguments: %s\n", strerror(error));
    argp_help(&argp, stderr, ARGP_HELP_SEE, argv[0]);
    exit(1);
  }

  if(arg_st == argc) {
    fprintf(stderr, "Missing arguments\n");
    argp_help(&argp, stderr, ARGP_HELP_SEE, argv[0]);
    exit(1);
  }

  if(arguments.mer_len == 0) {
    fprintf(stderr, "Specifying the mer size (-m) is required.\n");
    argp_help(&argp, stderr, ARGP_HELP_SEE, argv[0]);
    exit(1);
  }

  if(arguments.size == 0) {
    fprintf(stderr, "Specifying the hash table size (-s) is required.\nSee the man page for how to estimate this parameter.\n");
    argp_help(&argp, stderr, ARGP_HELP_SEE, argv[0]);
    exit(1);
  }

  Time start;
  mer_counting_base *counter;
  if(arguments.fastq) {
    counter = new mer_counting_fastq(arg_st, argc, argv, arguments);
  } else if(ceilLog2(arguments.size) > 2 * arguments.mer_len) {
    counter = new mer_counting_fasta_direct(arg_st, argc, argv, arguments);
  } else {
    counter = new mer_counting_fasta_hash(arg_st, argc, argv, arguments);
  }
  Time after_init;
  counter->count();
  Time all_done;

  if(arguments.timing) {
    std::ofstream timing_fd(arguments.timing);
    if(!timing_fd.good()) {
      fprintf(stderr, "Can't open timing file '%s': %s\n",
              arguments.timing, strerror(errno));
    } else {
      Time writing = counter->get_writing_time();
      Time counting = (all_done - after_init) - writing;
      timing_fd << "Init     " << (after_init - start).str() << "\n";
      timing_fd << "Counting " << counting.str() << "\n";
      timing_fd << "Writing  " << writing.str() << std::endl;
      timing_fd.close();
    }
  }

  return 0;
}
