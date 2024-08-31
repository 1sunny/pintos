#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>

static void read_line (char line[], size_t);
static bool backspace (char **pos, char line[]);
static bool getcwd (char *cwd, size_t cwd_size);

#define MAX_LEVEL 20

int
main (void)
{
  printf ("Shell starting...\n");
  for (;;) 
    {
      char command[80];

      char pwd[MAX_LEVEL * 3 + 1 + READDIR_MAX_LEN + 1];
      if (getcwd (pwd, sizeof pwd) == false) {
        printf ("Shell getcwd Error...\n");
        return EXIT_FAILURE;
      }
      /* Read command. */
      printf ("%s >> ", pwd);
      read_line (command, sizeof command);
      
      /* Execute command. */
      if (!strcmp (command, "exit"))
        break;
      else if (!memcmp (command, "cd ", 3)) 
        {
          if (!chdir (command + 3))
            printf ("\"%s\": chdir failed\n", command + 3);
        }
      else if (command[0] == '\0') 
        {
          /* Empty command. */
        }
      else
        {
          pid_t pid = exec (command);
          if (pid != PID_ERROR)
            printf ("\"%s\": exit code %d\n", command, wait (pid));
          else
            printf ("exec failed\n");
        }
    }

  printf ("Shell exiting.");
  return EXIT_SUCCESS;
}

/** Reads a line of input from the user into LINE, which has room
   for SIZE bytes.  Handles backspace and Ctrl+U in the ways
   expected by Unix users.  On return, LINE will always be
   null-terminated and will not end in a new-line character. */
static void
read_line (char line[], size_t size) 
{
  char *pos = line;
  for (;;)
    {
      char c;
      read (STDIN_FILENO, &c, 1);

      switch (c) 
        {
        case '\r':
          *pos = '\0';
          putchar ('\n');
          return;

        case '\b':
          backspace (&pos, line);
          break;

        case ('U' - 'A') + 1:       /**< Ctrl+U. */
          while (backspace (&pos, line))
            continue;
          break;

        default:
          /* Add character to line. */
          if (pos < line + size - 1) 
            {
              putchar (c);
              *pos++ = c;
            }
          break;
        }
    }
}

/** If *POS is past the beginning of LINE, backs up one character
   position.  Returns true if successful, false if nothing was
   done. */
static bool
backspace (char **pos, char line[]) 
{
  if (*pos > line)
    {
      /* Back up cursor, overwrite character, back up
         again. */
      printf ("\b \b");
      (*pos)--;
      return true;
    }
  else
    return false;
}

/** Stores the inode number for FILE_NAME in *INUM.
   Returns true if successful, false if the file could not be
   opened. */
static bool
get_inumber (const char *file_name, int *inum)
{
  int fd = open (file_name);
  if (fd >= 0)
  {
    *inum = inumber (fd);
    close (fd);
    return true;
  }
  else
    return false;
}

/** Prepends PREFIX to the characters stored in the final *DST_LEN
   bytes of the DST_SIZE-byte buffer that starts at DST.
   Returns true if successful, false if adding that many
   characters, plus a null terminator, would overflow the buffer.
   (No null terminator is actually added or depended upon, but
   its space is accounted for.) */
static bool
prepend (const char *prefix,
         char *dst, size_t *dst_len, size_t dst_size)
{
  size_t prefix_len = strlen (prefix);
  if (prefix_len + *dst_len + 1 <= dst_size)
  {
    *dst_len += prefix_len;
    memcpy ((dst + dst_size) - *dst_len, prefix, prefix_len);
    return true;
  }
  else
    return false;
}

/** Stores the current working directory, as a null-terminated
   string, in the CWD_SIZE bytes in CWD.
   Returns true if successful, false on error.  Errors include
   system errors, directory trees deeper than MAX_LEVEL levels,
   and insufficient space in CWD. */
static bool
getcwd (char *cwd, size_t cwd_size)
{
  size_t cwd_len = 0;

  char name[MAX_LEVEL * 3 + 1 + READDIR_MAX_LEN + 1];
  char *namep;

  int child_inum;

  /* Make sure there's enough space for at least "/". */
  if (cwd_size < 2)
    return false;

  /* Get inumber for current directory. */
  if (!get_inumber (".", &child_inum))
    return false;

  namep = name;
  for (;;)
  {
    int parent_inum, parent_fd;

    /* Compose "../../../..", etc., in NAME. */
    if ((namep - name) > MAX_LEVEL * 3)
      return false;
    *namep++ = '.';
    *namep++ = '.';
    *namep = '\0';

    /* Open directory. */
    parent_fd = open (name);
    if (parent_fd < 0)
      return false;
    *namep++ = '/';

    /* If parent and child have the same inumber,
       then we've arrived at the root. */
    parent_inum = inumber (parent_fd);
    if (parent_inum == child_inum)
      break;

    /* Find name of file in parent directory with the child's
       inumber. */
    for (;;)
    {
      int test_inum;
      if (!readdir (parent_fd, namep) || !get_inumber (name, &test_inum))
      {
        close (parent_fd);
        return false;
      }
      if (test_inum == child_inum)
        break;
    }
    close (parent_fd);

    /* Prepend "/name" to CWD. */
    if (!prepend (namep - 1, cwd, &cwd_len, cwd_size))
      return false;

    /* Move up. */
    child_inum = parent_inum;
  }

  /* Finalize CWD. */
  if (cwd_len > 0)
  {
    /* Move the string to the beginning of CWD,
       and null-terminate it. */
    memmove (cwd, (cwd + cwd_size) - cwd_len, cwd_len);
    cwd[cwd_len] = '\0';
  }
  else
  {
    /* Special case for the root. */
    strlcpy (cwd, "/", cwd_size);
  }

  return true;
}
