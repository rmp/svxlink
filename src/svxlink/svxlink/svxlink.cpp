/**
@file	 svxlink.cpp
@brief   The main file for the SvxLink server
@author  Tobias Blomberg / SM0SVX
@date	 2004-03-28

\verbatim
<A brief description of the program or library this file belongs to>
Copyright (C) 2003 Tobias Blomberg / SM0SVX

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
\endverbatim
*/



/****************************************************************************
 *
 * System Includes
 *
 ****************************************************************************/

#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <popt.h>
#include <locale.h>

#include <string>
#include <iostream>
#include <algorithm>


/****************************************************************************
 *
 * Project Includes
 *
 ****************************************************************************/

#include <AsyncCppApplication.h>
#include <AsyncConfig.h>
#include <AsyncTimer.h>
#include <AsyncFdWatch.h>

#include <version/SVXLINK.h>



/****************************************************************************
 *
 * Local Includes
 *
 ****************************************************************************/

#include "LocalRx.h"
#include "LocalTx.h"
#include "MsgHandler.h"
#include "SimplexLogic.h"
#include "RepeaterLogic.h"



/****************************************************************************
 *
 * Namespaces to use
 *
 ****************************************************************************/

using namespace std;
using namespace Async;
using namespace SigC;



/****************************************************************************
 *
 * Defines & typedefs
 *
 ****************************************************************************/

#define PROGRAM_NAME "SvxLink"



/****************************************************************************
 *
 * Local class definitions
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Prototypes
 *
 ****************************************************************************/

static void parse_arguments(int argc, const char **argv);
static void stdinHandler(FdWatch *w);
static void stdout_handler(FdWatch *w);
static void initialize_logics(Config &cfg);


/****************************************************************************
 *
 * Exported Global Variables
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Local Global Variables
 *
 ****************************************************************************/

static char   	*logfile_name = NULL;
static int    	daemonize = 0;
static FILE   	*logfile = 0;
static Logic  	*logic = 0;
static FdWatch	*stdin_watch = 0;
static FdWatch	*stdout_watch = 0;
static string   tstamp_format;


/****************************************************************************
 *
 * MAIN
 *
 ****************************************************************************/


/*
 *----------------------------------------------------------------------------
 * Function:  main
 * Purpose:   Start everything...
 * Input:     argc  - The number of arguments passed to this program
 *    	      	      (including the program name).
 *    	      argv  - The arguments passed to this program. argv[0] is the
 *    	      	      program name.
 * Output:    Return 0 on success, else non-zero.
 * Author:    Tobias Blomberg, SM0SVX
 * Created:   2004-03-28
 * Remarks:   
 * Bugs:      
 *----------------------------------------------------------------------------
 */
int main(int argc, char **argv)
{
  setlocale(LC_ALL, "");

  CppApplication app;

  parse_arguments(argc, const_cast<const char **>(argv));

  int pipefd[2] = {-1, -1};
  int noclose = 0;
  if (logfile_name != 0)
  {
      /* Open the logfile */
    logfile = fopen(logfile_name, "a");
    if (logfile == NULL)
    {
      perror("fopen(logfile)");
      exit(1);
    }

      /* Create a pipe to route stdout through */
    if (pipe(pipefd) == -1)
    {
      perror("pipe");
      exit(1);
    }
    stdout_watch = new FdWatch(pipefd[0], FdWatch::FD_WATCH_RD);
    stdout_watch->activity.connect(slot(&stdout_handler));

      /* Redirect stdout to the logpipe */
    if (close(STDOUT_FILENO) == -1)
    {
      perror("close(stdout)");
      exit(1);
    }
    if (dup2(pipefd[1], STDOUT_FILENO) == -1)
    {
      perror("dup2(stdout)");
      exit(1);
    }

      /* Redirect stderr to the logpipe */
    if (close(STDERR_FILENO) == -1)
    {
      perror("close(stderr)");
      exit(1);
    }
    if (dup2(pipefd[1], STDERR_FILENO) == -1)
    {
      perror("dup2(stderr)");
      exit(1);
    }

      /* Close stdin */
    close(STDIN_FILENO);
    
      /* Tell the daemon function call not to close the file descriptors */
    noclose = 1;
  }

  if (daemonize)
  {
    if (daemon(0, noclose) == -1)
    {
      perror("daemon");
      exit(1);
    }
  }

  char *home_dir = getenv("HOME");
  if (home_dir == NULL)
  {
    home_dir = ".";
  }
  
  tstamp_format = "%c";

  string cfg_filename(home_dir);
  cfg_filename += "/.svxlinkrc";
  Config cfg;
  if (cfg.open(cfg_filename))
  {
    cout << "Using config file: " << cfg_filename << endl;
  }
  else
  {
    cfg_filename = "/etc/svxlink.conf";
    if (cfg.open(cfg_filename))
    {
      cout << "Using config file: " << cfg_filename << endl;
    }
    else
    {
      cerr << "*** ERROR: Could not open config file. Tried both "
      	   << "\"" << home_dir << "/.svxlinkrc\" and "
	   << "\"/etc/svxlink.conf\". Possible reasons are: None of the files "
	   << "exist, you do not have permission to read the file or there "
	   << "was a syntax error in the file\n";
      exit(1);
    }
  }

  cfg.getValue("GLOBAL", "TIMESTAMP_FORMAT", tstamp_format);
  
  cout << PROGRAM_NAME " v" SVXLINK_VERSION " (" __DATE__ ") starting up...\n";
  
  initialize_logics(cfg);
  
  struct termios termios, org_termios;
  tcgetattr(STDIN_FILENO, &org_termios);
  termios = org_termios;
  termios.c_lflag &= ~(ICANON | ECHO);
  tcsetattr(STDIN_FILENO, TCSANOW, &termios);

  stdin_watch = new FdWatch(STDIN_FILENO, FdWatch::FD_WATCH_RD);
  stdin_watch->activity.connect(slot(&stdinHandler));

  app.exec();
  
  delete stdin_watch;
  tcsetattr(STDIN_FILENO, TCSANOW, &org_termios);

  if (stdout_watch != 0)
  {
    delete stdout_watch;
    close(pipefd[0]);
    close(pipefd[1]);
  }

  delete logic;
  
  if (logfile != 0)
  {
    fclose(logfile);
  }
  
  return 0;
  
} /* main */




/****************************************************************************
 *
 * Functions
 *
 ****************************************************************************/

/*
 *----------------------------------------------------------------------------
 * Function:  parse_arguments
 * Purpose:   Parse the command line arguments.
 * Input:     argc  - Number of arguments in the command line
 *    	      argv  - Array of strings with the arguments
 * Output:    Returns 0 if all is ok, otherwise -1.
 * Author:    Tobias Blomberg, SM0SVX
 * Created:   2000-06-13
 * Remarks:   
 * Bugs:      
 *----------------------------------------------------------------------------
 */
static void parse_arguments(int argc, const char **argv)
{
  poptContext optCon;
  const struct poptOption optionsTable[] =
  {
    POPT_AUTOHELP
    {"logfile", 0, POPT_ARG_STRING, &logfile_name, 0,
	    "Specify the logfile to use (stdout and stderr)", "<filename>"},
    /*
    {"int_arg", 'i', POPT_ARG_INT, &int_arg, 0,
	    "Description of int argument", "<an int>"},
    */
    {"daemon", 0, POPT_ARG_NONE, &daemonize, 0,
	    "Start SvxLink as a daemon", NULL},
    {NULL, 0, 0, NULL, 0}
  };
  int err;
  //const char *arg = NULL;
  //int argcnt = 0;
  
  optCon = poptGetContext(PROGRAM_NAME, argc, argv, optionsTable, 0);
  poptReadDefaultConfig(optCon, 0);
  
  err = poptGetNextOpt(optCon);
  if (err != -1)
  {
    fprintf(stderr, "\t%s: %s\n",
	    poptBadOption(optCon, POPT_BADOPTION_NOALIAS),
	    poptStrerror(err));
    exit(1);
  }

  /*
  printf("string_arg  = %s\n", string_arg);
  printf("int_arg     = %d\n", int_arg);
  printf("bool_arg    = %d\n", bool_arg);
  */
  
    /* Parse arguments that do not begin with '-' (leftovers) */
  /*
  arg = poptGetArg(optCon);
  while (arg != NULL)
  {
    printf("arg %2d      = %s\n", ++argcnt, arg);
    arg = poptGetArg(optCon);
  }
  */

  poptFreeContext(optCon);

} /* parse_arguments */


static void stdinHandler(FdWatch *w)
{
  char buf[1];
  int cnt = ::read(STDIN_FILENO, buf, 1);
  if (cnt == -1)
  {
    fprintf(stderr, "*** error reading from stdin\n");
    Application::app().quit();
    return;
  }
  else if (cnt == 0)
  {
      /* Stdin file descriptor closed */
    delete stdin_watch;
    stdin_watch = 0;
    return;
  }
  
  switch (toupper(buf[0]))
  {
    case 'Q':
      Application::app().quit();
      break;
    
    case '\n':
      putchar('\n');
      break;
    
    case '0': case '1': case '2': case '3':
    case '4': case '5': case '6': case '7':
    case '8': case '9': case 'A': case 'B':
    case 'C': case 'D': case '*': case '#':
      logic->dtmfDigitDetected(buf[0]);
      break;
    
    default:
      break;
  }
}


static void stdout_handler(FdWatch *w)
{
  char buf[256];
  ssize_t len = read(w->fd(), buf, sizeof(buf)-1);
  if (len == -1)
  {
    return;
  }
  buf[len] = 0;

  char *ptr = buf;
  while (*ptr != 0)
  {
    static bool print_timestamp = true;

    if (print_timestamp && !tstamp_format.empty())
    {
      time_t epoch = time(NULL);
      struct tm *tm = localtime(&epoch);
      char tstr[256];
      size_t tlen = strftime(tstr, sizeof(tstr), tstamp_format.c_str(), tm);
      fwrite(tstr, 1, tlen, logfile);
      fwrite(": ", 1, 2, logfile);
      print_timestamp = false;
    }

    int write_len = 0;
    char *nl = strchr(ptr, '\n');
    if (nl != 0)
    {
      write_len = nl-ptr+1;
      print_timestamp = true;
    }
    else
    {
      write_len = strlen(ptr);
    }
    fwrite(ptr, 1, write_len, logfile);
    ptr += write_len;
    fflush(logfile);
  }
} /* stdout_handler  */


static void initialize_logics(Config &cfg)
{
  string logics;
  if (!cfg.getValue("GLOBAL", "LOGICS", logics) || logics.empty())
  {
    cerr << "*** ERROR: Config variable GLOBAL/LOGICS is not set\n";
    exit(1);
  }

  string::iterator comma;
  string::iterator begin = logics.begin();
  do
  {
    comma = find(begin, logics.end(), ',');
    string logic_name;
    if (comma == logics.end())
    {
      logic_name = string(begin, logics.end());
    }
    else
    {
      logic_name = string(begin, comma);
      begin = comma + 1;
    }
    
    cout << "\nStarting logic: " << logic_name << endl;
    
    string logic_type;
    if (!cfg.getValue(logic_name, "TYPE", logic_type) || logic_type.empty())
    {
      cerr << "*** ERROR: Logic TYPE not specified for logic \""
      	   << logic_name << "\". Skipping...\n";
      continue;
    }
    if (logic_type == "Simplex")
    {
      logic = new SimplexLogic(cfg, logic_name);
    }
    else if (logic_type == "Repeater")
    {
      logic = new RepeaterLogic(cfg, logic_name);
    }
    else
    {
      cerr << "*** ERROR: Unknown logic type \"" << logic_type
      	   << "\"specified.\n";
      continue;
    }
    if (!logic->initialize())
    {
      cerr << "*** ERROR: Could not initialize Logic object \""
      	   << logic_name << "\". Skipping...\n";
      delete logic;
    }
    
  } while (comma != logics.end());
}



/*
 * This file has not been truncated
 */

