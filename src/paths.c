/*
  Copyright 2012-2018 Jyri J. Virkki <jyri@virkki.com>

  This file is part of dupd.

  dupd is free software: you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  dupd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with dupd.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "dirtree.h"
#include "main.h"
#include "paths.h"
#include "readlist.h"
#include "sizelist.h"
#include "stats.h"
#include "utils.h"

// Path lists (head + entries) are stored in path blocks which are preallocated
// as needed. This list holds the blocks we've had to allocate.

struct path_block_list {
  char * ptr;
  struct path_block_list * next;
};

static struct path_block_list * first_path_block = NULL;
static struct path_block_list * last_path_block = NULL;
static char * next_entry;
static char * path_block_end;
static long space_used;
static long space_allocated;
void * fiemap = NULL;


/** ***************************************************************************
 * Debug function. Dumps the path list for a given size starting from head.
 *
 */
void dump_path_list(const char * line, uint64_t size,
                    struct path_list_head * head, int dump_all)
{
  printf("----- dump path block list for size %ld -----\n", (long)size);
  printf("%s\n", line);

  printf("  head: %p\n", head);
  printf("  last_elem: %p\n", head->last_entry);
  printf("  list_size: %d\n", head->list_size);
  printf("  state: %s\n", pls_state(head->state));
  printf("  sizelist back ptr: %p\n", head->sizelist);

  if (head->sizelist != NULL) {
    printf("   forward ptr back to me: %p\n", head->sizelist->path_list);
    if (head->sizelist->path_list != head) {                 // LCOV_EXCL_START
      printf("error: mismatch!\n");
      exit(1);
    }                                                        // LCOV_EXCL_STOP
  }

  struct path_list_entry * entry = pb_get_first_entry(head);
  printf("  first_elem: %p\n", entry);

  uint32_t counted = 1;
  uint32_t valid = 0;
  char buffer[DUPD_PATH_MAX];
  char * filename;

  while (entry != NULL) {
    if (counted < 2 || log_level >= L_TRACE || dump_all) {
      printf(" --entry %d\n", counted);
      printf("   file state: %s\n", file_state(entry->state));
      printf("   filename_size: %d\n", entry->filename_size);
      printf("   dir: %p\n", entry->dir);
      printf("   next: %p\n", entry->next);
      printf("   buffer: %p\n", entry->buffer);
      printf("   blocks: %p\n", entry->blocks);
      dump_block_list("      ", entry->blocks);

      filename = pb_get_filename(entry);
      if (filename[0] != 0) { // TODO
        bzero(buffer, DUPD_PATH_MAX);
        memcpy(buffer, filename, entry->filename_size);
        buffer[entry->filename_size] = 0;
        printf("   filename (direct read): [%s]\n", buffer);
        bzero(buffer, DUPD_PATH_MAX);
        build_path(entry, buffer);
        printf("   built path: [%s]\n", buffer);
        if (entry->state != FS_INVALID) { valid++; }
      } else {
        printf("   filename: REMOVED EARLIER\n"); // TODO shouldn't exist
      }
    }
    counted++;
    entry = entry->next;
  }

  counted--;
  printf("counted entries: %d\n", counted);
  printf("valid entries: %d\n", valid);
  if (valid != head->list_size) {
                                                             // LCOV_EXCL_START
    printf("list_len (%d)!=valid entries (%d)\n", head->list_size, valid);
    exit(1);
  }                                                          // LCOV_EXCL_STOP

  printf("-----\n\n\n");
}


/** ***************************************************************************
 * Allocate a path block.
 *
 */
static struct path_block_list * alloc_path_block(int bsize)
{
  struct path_block_list * next;

  next = (struct path_block_list *)malloc(sizeof(struct path_block_list));
  next->next = NULL;
  next->ptr = (char *)malloc(bsize);

  if (next->ptr == NULL) {                                  // LCOV_EXCL_START
    printf("Unable to allocate new path block!\n");
    exit(1);
  }                                                          // LCOV_EXCL_STOP

  LOG(L_RESOURCES, "Allocated %d bytes for the next path block.\n", bsize);

  return next;
}


/** ***************************************************************************
 * Add another path block.
 *
 */
static void add_path_block()
{
  int bsize = 8 * 1024 * 1024;
  if (x_small_buffers) { bsize = DUPD_PATH_MAX; }

  struct path_block_list * next = alloc_path_block(bsize);
  next_entry = next->ptr;
  path_block_end = next->ptr + bsize;
  last_path_block->next = next;
  last_path_block = next;

  space_allocated += bsize;
}


/** ***************************************************************************
 * Check if current path block can accomodate 'needed' bytes. If not, add
 * another path block.
 *
 */
inline static void check_space(int needed)
{
  if (path_block_end - next_entry - 2 <= needed) {
    add_path_block();
  }
}


/** ***************************************************************************
 * Public function, see paths.h
 *
 */
void init_path_block()
{
  int bsize = 1024 * 1024;
  if (x_small_buffers) { bsize = DUPD_PATH_MAX; }

  first_path_block = alloc_path_block(bsize);
  next_entry = first_path_block->ptr;
  path_block_end = first_path_block->ptr + bsize;
  last_path_block = first_path_block;

  space_used = 0;
  space_allocated = bsize;

  if (using_fiemap) {
    fiemap = fiemap_alloc();
  }
}


/** ***************************************************************************
 * Public function, see paths.h
 *
 */
void free_path_block()
{
  struct path_block_list * b;
  struct path_block_list * p;

  p = first_path_block;
  while (p != NULL) {
    free(p->ptr);
    b = p;
    p = b->next;
    free(b);
  }
  first_path_block = NULL;
  last_path_block = NULL;

  if (fiemap != NULL) {
    free(fiemap);
  }
}


/** ***************************************************************************
 * Public function, see paths.h
 *
 */
struct path_list_head * insert_first_path(char * filename,
                                          struct direntry * dir_entry)
{
  int filename_len = strlen(filename);

  int space_needed = filename_len +
    sizeof(struct path_list_head) + sizeof(struct path_list_entry);

  check_space(space_needed);
  space_used += space_needed;

  // The new list head will live at the top of the available space (next_entry)
  struct path_list_head * head = (struct path_list_head *)next_entry;

  // The first entry will live immediately after the head
  struct path_list_entry * first_entry =
    (struct path_list_entry *)((char *)head + sizeof(struct path_list_head));

  // And the filename of first entry lives immediately after its entry
  char * filebuf = pb_get_filename(first_entry);

  // Move the free space pointer forward for the amount of space we took above
  next_entry += space_needed;

  // The associated sizelist does not exist yet
  head->sizelist = NULL;

  // Last entry is the first entry since there's only one now
  head->last_entry = first_entry;

  // Initialize list size to 1
  head->list_size = 1;

  // New path list
  head->state = PLS_NEW;

  // Initialize the first entry
  first_entry->state = FS_NEW;
  first_entry->filename_size = (uint8_t)filename_len;
  first_entry->dir = dir_entry;
  first_entry->blocks = NULL;
  first_entry->next = NULL;
  first_entry->buffer = NULL;
  memcpy(filebuf, filename, filename_len);

  LOG_TRACE {
    dump_path_list("AFTER insert_first_path", -1, head, 0);
  }

  stats_path_list_entries++;

  return head;
}


/** ***************************************************************************
 * Public function, see paths.h
 *
 */
void insert_end_path(char * filename, struct direntry * dir_entry,
                     ino_t inode, uint64_t size, struct path_list_head * head)
{
  char pathbuf[DUPD_PATH_MAX];
  struct block_list * block_list = NULL;

  int filename_len = strlen(filename);
  int space_needed = sizeof(struct path_list_entry) + filename_len;
  check_space(space_needed);
  space_used += space_needed;

  LOG_MORE_TRACE {
    dump_path_list("BEFORE insert_end_path", size, head, 0);
  }

  // The entry will live at the top of the available space (next_entry)
  struct path_list_entry * entry = (struct path_list_entry *)next_entry;

  // And the filename of first entry lives immediately after its entry
  char * filebuf = pb_get_filename(entry);

  // Move the free space pointer forward for the amount of space we took above
  next_entry += space_needed;

  // Last entry in this list is now this one and the list grew by one
  struct path_list_entry * prior = head->last_entry;
  head->last_entry = entry;
  prior->next = entry;
  head->list_size++;

  // Initialize this new entry
  entry->state = FS_NEW;
  entry->filename_size = (uint8_t)filename_len;
  entry->dir = dir_entry;
  entry->next = NULL;
  entry->buffer = NULL;
  entry->blocks = NULL;
  memcpy(filebuf, filename, filename_len);

  // If there are now two entries in this path list, it means we have
  // just identified a size which is a candidate for duplicate
  // processing later, so add it to the size list now.

  if (head->list_size == 2) {
    struct size_list * new_szl = add_to_size_list(size, head);
    head->sizelist = new_szl;

    if (hdd_mode) {
      STRUCT_STAT info;

      // Add the first entry to the read list. It wasn't added earlier
      // because we didn't know it needed to be there but now we do.
      // We'll need to re-stat() it to get info. This should be fast
      // because it should be in the cache already. (Alternatively,
      // could keep this info in the path list head.)

      build_path(prior, pathbuf);
      if (get_file_info(pathbuf, &info)) {                   // LCOV_EXCL_START
        printf("error: unable to stat %s\n", pathbuf);
        exit(1);
      }                                                      // LCOV_EXCL_STOP

      block_list = get_block_info_from_path(pathbuf, info.st_ino, size,fiemap);
      prior->blocks = block_list;
      add_to_read_list(head, prior, info.st_ino);
    }
  }

  if (hdd_mode) {
    build_path_from_string(filename, dir_entry, pathbuf);
    block_list = get_block_info_from_path(pathbuf, inode, size, fiemap);
    entry->blocks = block_list;
    add_to_read_list(head, entry, inode);
  }

  LOG_TRACE {
    dump_path_list("AFTER insert_end_path", size, head, 0);
  }

  if (head->list_size > stats_max_pathlist) {
    stats_max_pathlist = head->list_size;
    stats_max_pathlist_size = size;
  }

  stats_path_list_entries++;
}


/** ***************************************************************************
 * Public function, see paths.h
 *
 */
void report_path_block_usage()
{
  int pct = (int)((100 * space_used) / space_allocated);
  printf("Total path block size: %ld\n", space_allocated);
  printf("Bytes used in this run: %ld (%d%%)\n", space_used, pct);
  printf("Total files in path list: %" PRIu32 "\n", stats_path_list_entries);
}


/** ***************************************************************************
 * Public function, see paths.h
 *
 */
const char * pls_state(int state)
{
  switch(state) {
  case PLS_NEW:                      return "PLS_NEW";
  case PLS_R1_IN_PROGRESS:           return "PLS_R1_IN_PROGRESS";
  case PLS_R1_BUFFERS_FULL:          return "PLS_R1_BUFFERS_FULL";
  case PLS_R2_NEEDED:                return "PLS_R2_NEEDED";
  case PLS_DONE:                     return "PLS_DONE";
  default:
    printf("\nerror: unknown pls_state %d\n", state);
    exit(1);
  }
}


/** ***************************************************************************
 * Public function, see paths.h
 *
 */
const char * file_state(int state)
{
  switch(state) {
  case FS_NEW:                        return "FS_NEW";
  case FS_R1_BUFFER_FILLED:           return "FS_R1_BUFFER_FILLED";
  case FS_INVALID:                    return "FS_INVALID";
  case FS_R1_DONE:                    return "FS_R1_DONE";
  default:
    printf("\nerror: unknown file_state %d\n", state);
    exit(1);
  }
}


/** ***************************************************************************
 * Public function, see paths.h
 *
 */
int mark_path_entry_invalid(struct path_list_head * head,
                            struct path_list_entry * entry)
{
  entry->state = FS_INVALID;
  head->list_size--;
  LOG(L_TRACE, "Reduced list size to %d\n", head->list_size);

  { // TODO
    char * fname = pb_get_filename(entry);
    fname[0] = 0;
  }

  // If this path list is down to one file, nothing to see here.
  // But need to find that remaining file to mark it invalid as well.
  if (head->list_size == 1) {
    head->state = PLS_DONE;
    head->list_size = 0;
    d_mutex_lock(&stats_lock, "mark invalid stats");
    stats_sets_dup_not[ROUND1]++;
    d_mutex_unlock(&stats_lock);
    LOG(L_TRACE, "Reduced list size to %d, state now DONE\n", head->list_size);

    struct path_list_entry * e = pb_get_first_entry(head);
    int good = 0;

    while (e != NULL) {
      switch (e->state) {

      case FS_NEW:
        e->state = FS_INVALID;
        good++;
        break;

      case FS_R1_BUFFER_FILLED:
        if (e->buffer == NULL) {
          printf("error: null buffer but FS_R1_BUFFER_FILLED\n");
          dump_path_list("bad state", head->sizelist->size, head, 0);
          exit(1);
        }
        free(e->buffer);
        e->buffer = NULL;
        dec_stats_read_buffers_allocated(head->sizelist->bytes_read);
        good++;
        break;

      case FS_INVALID:
        break;

      case FS_R1_DONE:
        printf("error: unexpected state FS_R1_DONE in unfinished pathlist\n");
        dump_path_list("bad state", head->sizelist->size, head, 0);
        exit(1);
        break;

      default:
        printf("error: invalid state seen in mark_path_entry_invalid\n");
        dump_path_list("bad state", head->sizelist->size, head, 0);
        exit(1);
        break;
      }

      e = e->next;
    }

    if (good != 1) {
      printf("error: mark_path_entry_invalid wrong count of good paths\n");
      dump_path_list("bad state", head->sizelist->size, head, 0);
      exit(1);
    }
  }

  return head->list_size;
}
