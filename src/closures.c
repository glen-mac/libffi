/* -----------------------------------------------------------------------
   closures.c - Copyright (c) 2019 Anthony Green
                Copyright (c) 2007, 2009, 2010 Red Hat, Inc.
                Copyright (C) 2007, 2009, 2010 Free Software Foundation, Inc
                Copyright (c) 2011 Plausible Labs Cooperative, Inc.

   Code to allocate and deallocate memory for closures.

   Permission is hereby granted, free of charge, to any person obtaining
   a copy of this software and associated documentation files (the
   ``Software''), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish,
   distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to
   the following conditions:

   The above copyright notice and this permission notice shall be included
   in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED ``AS IS'', WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
   NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
   HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
   WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
   DEALINGS IN THE SOFTWARE.
   ----------------------------------------------------------------------- */

#if defined __linux__ && !defined _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <fficonfig.h>
#include <ffi.h>
#include <ffi_common.h>
#include <stdlib.h>

#include <mach/mach.h>

extern
kern_return_t mach_vm_region_recurse
(
	vm_map_read_t target_task,
	mach_vm_address_t *address,
	mach_vm_size_t *size,
	natural_t *nesting_depth,
	vm_region_recurse_info_t info,
	mach_msg_type_number_t *infoCnt
);

#include <os/log.h>
#include <sys/mman.h>
static
 void
    get_region_protection
    (
      vm_address_t                   Address
    )
{
    kern_return_t                            kStatus           = KERN_FAILURE;
    mach_vm_address_t                        regionBase        = (mach_vm_address_t) Address;
    mach_vm_size_t                           regionSize        = 0;
    natural_t                                nestingLevel      = 0;
    vm_region_submap_short_info_data_64_t    regionInfo        = { 0 };
    mach_msg_type_number_t                   regionInfoSize    = VM_REGION_SUBMAP_INFO_COUNT_64;

    kStatus = mach_vm_region_recurse( mach_task_self( ),
                                      &regionBase,
                                      &regionSize,
                                      &nestingLevel,
                                      (vm_region_recurse_info_t) &regionInfo,
                                      &regionInfoSize );
    if( KERN_SUCCESS == kStatus )
    {
        vm_prot_t Protection = regionInfo.protection;
        vm_prot_t ProtectionMax = regionInfo.max_protection;

        os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_ERROR, "region @ %p has perms: %c%c%c/%c%c%c\n",
                         (void*)regionBase,
                         (PROT_READ & Protection)  ? 'r' : '-',
                         (PROT_WRITE & Protection) ? 'w' : '-',
                         (PROT_EXEC & Protection) ?  'x' : '-',
                         (PROT_READ & ProtectionMax)  ? 'r' : '-',
                         (PROT_WRITE & ProtectionMax) ? 'w' : '-',
                         (PROT_EXEC & ProtectionMax) ?  'x' : '-'
        );
    }
    else
    {
        os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_ERROR, "ERROR mach_vm_region_recurse failed with result 0x%x ( %s )\n",
                    kStatus, mach_error_string( kStatus ) );
    }
}

static void
on_allocate (void *base_address, size_t size)
{
}

static void
on_deallocate (void *base_address, size_t size)
{
}

static ffi_mem_callbacks mem_callbacks = {
  malloc,
  calloc,
  free,
  on_allocate,
  on_deallocate
};

void
ffi_set_mem_callbacks (const ffi_mem_callbacks *callbacks)
{
  mem_callbacks = *callbacks;
}

#ifdef __NetBSD__
#include <sys/param.h>
#endif

#if __NetBSD_Version__ - 0 >= 799007200
/* NetBSD with PROT_MPROTECT */
#include <sys/mman.h>

#include <stddef.h>
#include <unistd.h>
#ifdef  HAVE_SYS_MEMFD_H
#include <sys/memfd.h>
#endif

static const size_t overhead =
  (sizeof(max_align_t) > sizeof(void *) + sizeof(size_t)) ?
    sizeof(max_align_t)
    : sizeof(void *) + sizeof(size_t);

#define ADD_TO_POINTER(p, d) ((void *)((uintptr_t)(p) + (d)))

void *
ffi_closure_alloc (size_t size, void **code)
{
  static size_t page_size;
  size_t rounded_size;
  void *codeseg, *dataseg;
  int prot;

  /* Expect that PAX mprotect is active and a separate code mapping is necessary. */
  if (!code)
    return NULL;

  /* Obtain system page size. */
  if (!page_size)
    page_size = sysconf(_SC_PAGESIZE);

  /* Round allocation size up to the next page, keeping in mind the size field and pointer to code map. */
  rounded_size = (size + overhead + page_size - 1) & ~(page_size - 1);

  /* Primary mapping is RW, but request permission to switch to PROT_EXEC later. */
  prot = PROT_READ | PROT_WRITE | PROT_MPROTECT(PROT_EXEC);
  dataseg = mmap(NULL, rounded_size, prot, MAP_ANON | MAP_PRIVATE, -1, 0);
  if (dataseg == MAP_FAILED)
    return NULL;

  /* Create secondary mapping and switch it to RX. */
  codeseg = mremap(dataseg, rounded_size, NULL, rounded_size, MAP_REMAPDUP);
  if (codeseg == MAP_FAILED) {
    munmap(dataseg, rounded_size);
    return NULL;
  }
  if (mprotect(codeseg, rounded_size, PROT_READ | PROT_EXEC) == -1) {
    munmap(codeseg, rounded_size);
    munmap(dataseg, rounded_size);
    return NULL;
  }

  mem_callbacks.on_allocate (dataseg, rounded_size);
  mem_callbacks.on_allocate (codeseg, rounded_size);

  /* Remember allocation size and location of the secondary mapping for ffi_closure_free. */
  memcpy(dataseg, &rounded_size, sizeof(rounded_size));
  memcpy(ADD_TO_POINTER(dataseg, sizeof(size_t)), &codeseg, sizeof(void *));
  *code = ADD_TO_POINTER(codeseg, overhead);
  return ADD_TO_POINTER(dataseg, overhead);
}

void
ffi_closure_free (void *ptr)
{
  void *codeseg, *dataseg;
  size_t rounded_size;

  dataseg = ADD_TO_POINTER(ptr, -overhead);
  memcpy(&rounded_size, dataseg, sizeof(rounded_size));
  memcpy(&codeseg, ADD_TO_POINTER(dataseg, sizeof(size_t)), sizeof(void *));
  munmap(dataseg, rounded_size);
  munmap(codeseg, rounded_size);

  mem_callbacks.on_deallocate (codeseg, rounded_size);
  mem_callbacks.on_deallocate (dataseg, rounded_size);
}
#else /* !NetBSD with PROT_MPROTECT */

#if !FFI_MMAP_EXEC_WRIT && !FFI_EXEC_TRAMPOLINE_TABLE
# if __linux__ && !defined(__ANDROID__)
/* This macro indicates it may be forbidden to map anonymous memory
   with both write and execute permission.  Code compiled when this
   option is defined will attempt to map such pages once, but if it
   fails, it falls back to creating a temporary file in a writable and
   executable filesystem and mapping pages from it into separate
   locations in the virtual memory space, one location writable and
   another executable.  */
#  define FFI_MMAP_EXEC_WRIT 1
#  define HAVE_MNTENT 1
# endif
# if defined(_WIN32) || defined(__OS2__)
/* Windows systems may have Data Execution Protection (DEP) enabled, 
   which requires the use of VirtualMalloc/VirtualFree to alloc/free
   executable memory. */
#  define FFI_MMAP_EXEC_WRIT 1
# endif
#endif

#if FFI_CLOSURES

#if FFI_EXEC_TRAMPOLINE_TABLE

#ifdef __MACH__

#include <mach/mach.h>
#include <pthread.h>
#ifdef HAVE_PTRAUTH
#include <ptrauth.h>
#endif
#include <stdio.h>
#include <stdlib.h>

extern void *ffi_closure_trampoline_table_page;

typedef struct ffi_trampoline_table ffi_trampoline_table;
typedef struct ffi_trampoline_table_entry ffi_trampoline_table_entry;

struct ffi_trampoline_table
{
  /* contiguous writable and executable pages */
  vm_address_t config_page;
  vm_address_t trampoline_page;

  /* free list tracking */
  uint16_t free_count;
  ffi_trampoline_table_entry *free_list;
  ffi_trampoline_table_entry *free_list_pool;

  ffi_trampoline_table *prev;
  ffi_trampoline_table *next;
};

struct ffi_trampoline_table_entry
{
  void *(*trampoline) (void);
  ffi_trampoline_table_entry *next;
};

/* Total number of trampolines that fit in one trampoline table */
#define FFI_TRAMPOLINE_COUNT (PAGE_MAX_SIZE / FFI_TRAMPOLINE_SIZE)

static void ffi_trampoline_table_free (ffi_trampoline_table *table);

static pthread_mutex_t ffi_trampoline_lock = PTHREAD_MUTEX_INITIALIZER;
static ffi_trampoline_table *ffi_trampoline_tables = NULL;

void
ffi_deinit (void)
{
  while (ffi_trampoline_tables != NULL)
    {
      ffi_trampoline_table *table = ffi_trampoline_tables;
      ffi_trampoline_tables = table->next;
      ffi_trampoline_table_free (table);
    }

  pthread_mutex_destroy (&ffi_trampoline_lock);
}

static ffi_trampoline_table *
ffi_trampoline_table_alloc (void)
{
  ffi_trampoline_table *table;
  vm_address_t config_page;
  vm_address_t trampoline_page;
  vm_address_t trampoline_page_template;
  /*vm_prot_t cur_prot;*/
  /*vm_prot_t max_prot;*/
  kern_return_t kt;
  kern_return_t ktt;
  uint16_t i;

  /* Allocate two pages -- a config page and a placeholder page */
  config_page = 0x0;
  kt = vm_allocate (mach_task_self (), &config_page, PAGE_MAX_SIZE * 2,
		    VM_FLAGS_ANYWHERE);
  if (kt != KERN_SUCCESS)
    return NULL;

    os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_ERROR, "getting perms of config page:\n" );
    get_region_protection( config_page );

  /* Remap the trampoline table on top of the placeholder page */
  trampoline_page = config_page + PAGE_MAX_SIZE;
  trampoline_page_template = (vm_address_t)&ffi_closure_trampoline_table_page;
#ifdef __arm__
  /* ffi_closure_trampoline_table_page can be thumb-biased on some ARM archs */
  trampoline_page_template &= ~1UL;
#endif

/*cur_prot = VM_PROT_READ | VM_PROT_EXECUTE;*/
/*max_prot = VM_PROT_READ | VM_PROT_EXECUTE;*/

    // at this point trampoline_page is rw-/rwx and the template is r-x/rwx
    os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_ERROR, "getting perms of trampoline_page_template:\n" );
    get_region_protection( trampoline_page_template );

    os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_ERROR, "mach_vm_write from template to page\n" );
    ktt = vm_write(mach_task_self(), trampoline_page, trampoline_page_template, PAGE_MAX_SIZE);
    os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_ERROR, "ret EGG: %s", mach_error_string( ktt )  );
    get_region_protection( trampoline_page );

    // at this point trampoline_page is r-x/rwx and the template is r-x/rwx
    os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_ERROR, "initial vm_protect to set R-X/RWX\n" );
    ktt = vm_protect ( mach_task_self(), trampoline_page, PAGE_MAX_SIZE, FALSE, VM_PROT_READ | VM_PROT_EXECUTE );
    os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_ERROR, "ret EGG: %s", mach_error_string( ktt )  );
    get_region_protection( trampoline_page );

  /*kt = vm_remap (mach_task_self (), &trampoline_page, PAGE_MAX_SIZE, 0x0,*/
		 /*VM_FLAGS_OVERWRITE, mach_task_self (), trampoline_page_template,*/
		 /*FALSE, &cur_prot, &max_prot, VM_INHERIT_SHARE);*/
  /*if (kt != KERN_SUCCESS)*/
    /*{*/
      /*vm_deallocate (mach_task_self (), config_page, PAGE_MAX_SIZE * 2);*/
      /*return NULL;*/
    /*}*/

    /*os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_ERROR, "getting perms of trampoline_page:\n" );*/
    /*get_region_protection( trampoline_page );*/

    /*os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_ERROR, "now doing mach_vm_protect:\n" );*/
    /*ktt = vm_protect ( mach_task_self(), trampoline_page, PAGE_MAX_SIZE, FALSE, VM_PROT_READ | VM_PROT_EXECUTE );*/
    /*get_region_protection( trampoline_page );*/
    /*os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_ERROR, "ret EGG: %s", mach_error_string( ktt )  );*/


    // at this point trampoline_page SHOULD be r-x/rwx and the template is r-x/rwx
    /*os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_ERROR, "mach_vm_remap from template to page\n" );*/
    /*ktt = vm_remap (mach_task_self(), &trampoline_page, PAGE_MAX_SIZE, PAGE_MASK,*/
        /*VM_FLAGS_OVERWRITE | VM_FLAGS_FIXED, mach_task_self(), trampoline_page_template, FALSE,*/
        /*&cur_prot, &max_prot, VM_INHERIT_SHARE );*/
    /*os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_ERROR, "ret EGG: %s", mach_error_string( ktt )  );*/

    /*if (ktt == KERN_NO_SPACE)*/
    /*{*/
    /*os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_ERROR, "mach_vm_protect: again\n" );*/
      /*ktt = vm_protect (mach_task_self(), trampoline_page, PAGE_MAX_SIZE, FALSE,*/
          /*VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);*/
    /*get_region_protection( trampoline_page );*/
    /*os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_ERROR, "ret EGG: %s", mach_error_string( ktt )  );*/

    /*os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_ERROR, "mach_vm_remap again\n" );*/
      /*ktt = vm_remap (mach_task_self(), &trampoline_page, PAGE_MAX_SIZE, 0,*/
          /*VM_FLAGS_OVERWRITE | VM_FLAGS_FIXED, mach_task_self(), trampoline_page_template, TRUE,*/
          /*&cur_prot, &max_prot, VM_INHERIT_COPY);*/
    /*get_region_protection( trampoline_page );*/
    /*os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_ERROR, "ret EGG: %s", mach_error_string( ktt )  );*/

    /*os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_ERROR, "mach_vm_protect final\n" );*/
      /*ktt = vm_protect (mach_task_self(), trampoline_page, PAGE_MAX_SIZE, FALSE,*/
          /*VM_PROT_READ | VM_PROT_EXECUTE);*/
    /*get_region_protection( trampoline_page );*/
    /*os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_ERROR, "ret EGG: %s", mach_error_string( ktt )  );*/
    /*}*/

  mem_callbacks.on_allocate ((void *) config_page, PAGE_MAX_SIZE * 2);

  /* We have valid trampoline and config pages */
  table = mem_callbacks.calloc (1, sizeof (ffi_trampoline_table));
  table->free_count = FFI_TRAMPOLINE_COUNT;
  table->config_page = config_page;
  table->trampoline_page = trampoline_page;

  /* Create and initialize the free list */
  table->free_list_pool =
    mem_callbacks.calloc (FFI_TRAMPOLINE_COUNT, sizeof (ffi_trampoline_table_entry));

  for (i = 0; i < table->free_count; i++)
    {
      ffi_trampoline_table_entry *entry = &table->free_list_pool[i];
      entry->trampoline =
	(void *) (table->trampoline_page + (i * FFI_TRAMPOLINE_SIZE));

      if (i < table->free_count - 1)
	entry->next = &table->free_list_pool[i + 1];
    }

  table->free_list = table->free_list_pool;

  return table;
}

static void
ffi_trampoline_table_free (ffi_trampoline_table *table)
{
  /* Remove from the list */
  if (table->prev != NULL)
    table->prev->next = table->next;

  if (table->next != NULL)
    table->next->prev = table->prev;

  /* Deallocate pages */
  vm_deallocate (mach_task_self (), table->config_page, PAGE_MAX_SIZE * 2);
  mem_callbacks.on_deallocate ((void *) table->config_page, PAGE_MAX_SIZE * 2);

  /* Deallocate free list */
  mem_callbacks.free (table->free_list_pool);
  mem_callbacks.free (table);
}

void *
ffi_closure_alloc (size_t size, void **code)
{
  /* Create the closure */
  ffi_closure *closure = mem_callbacks.malloc (size);
  if (closure == NULL)
    return NULL;

  pthread_mutex_lock (&ffi_trampoline_lock);

  /* Check for an active trampoline table with available entries. */
  ffi_trampoline_table *table = ffi_trampoline_tables;
  if (table == NULL || table->free_list == NULL)
    {
      table = ffi_trampoline_table_alloc ();
      if (table == NULL)
	{
	  pthread_mutex_unlock (&ffi_trampoline_lock);
	  mem_callbacks.free (closure);
	  return NULL;
	}

      /* Insert the new table at the top of the list */
      table->next = ffi_trampoline_tables;
      if (table->next != NULL)
	table->next->prev = table;

      ffi_trampoline_tables = table;
    }

  /* Claim the free entry */
  ffi_trampoline_table_entry *entry = ffi_trampoline_tables->free_list;
  ffi_trampoline_tables->free_list = entry->next;
  ffi_trampoline_tables->free_count--;
  entry->next = NULL;

  pthread_mutex_unlock (&ffi_trampoline_lock);

  /* Initialize the return values */
  *code = entry->trampoline;
#ifdef HAVE_PTRAUTH
  *code = ptrauth_sign_unauthenticated (*code, ptrauth_key_asia, 0);
#endif
  closure->trampoline_table = table;
  closure->trampoline_table_entry = entry;

  return closure;
}

void
ffi_closure_free (void *ptr)
{
  ffi_closure *closure = ptr;

  pthread_mutex_lock (&ffi_trampoline_lock);

  /* Fetch the table and entry references */
  ffi_trampoline_table *table = closure->trampoline_table;
  ffi_trampoline_table_entry *entry = closure->trampoline_table_entry;

  /* Return the entry to the free list */
  entry->next = table->free_list;
  table->free_list = entry;
  table->free_count++;

  /* If all trampolines within this table are free, and at least one other table exists, deallocate
   * the table */
  if (table->free_count == FFI_TRAMPOLINE_COUNT
      && ffi_trampoline_tables != table)
    {
      ffi_trampoline_table_free (table);
    }
  else if (ffi_trampoline_tables != table)
    {
      /* Otherwise, bump this table to the top of the list */
      table->prev = NULL;
      table->next = ffi_trampoline_tables;
      if (ffi_trampoline_tables != NULL)
	ffi_trampoline_tables->prev = table;

      ffi_trampoline_tables = table;
    }

  pthread_mutex_unlock (&ffi_trampoline_lock);

  /* Free the closure */
  mem_callbacks.free (closure);
}

#endif

// Per-target implementation; It's unclear what can reasonable be shared between two OS/architecture implementations.

#elif FFI_MMAP_EXEC_WRIT /* !FFI_EXEC_TRAMPOLINE_TABLE */

#define USE_LOCKS 1
#define USE_DL_PREFIX 1
#ifdef __GNUC__
#ifndef USE_BUILTIN_FFS
#define USE_BUILTIN_FFS 1
#endif
#endif

/* We need to use mmap, not sbrk.  */
#define HAVE_MORECORE 0

/* We could, in theory, support mremap, but it wouldn't buy us anything.  */
#define HAVE_MREMAP 0

/* We have no use for this, so save some code and data.  */
#define NO_MALLINFO 1

/* We need all allocations to be in regular segments, otherwise we
   lose track of the corresponding code address.  */
#define DEFAULT_MMAP_THRESHOLD MAX_SIZE_T

/* Don't allocate more than a page unless needed.  */
#define DEFAULT_GRANULARITY ((size_t)malloc_getpagesize)

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#ifndef _MSC_VER
#include <unistd.h>
#endif
#include <string.h>
#include <stdio.h>
#if !defined(_WIN32)
#ifdef HAVE_MNTENT
#include <mntent.h>
#endif /* HAVE_MNTENT */
#include <sys/param.h>
#include <pthread.h>

/* We don't want sys/mman.h to be included after we redefine mmap and
   dlmunmap.  */
#include <sys/mman.h>
#define LACKS_SYS_MMAN_H 1

#define is_selinux_enabled() 0

#elif defined (__CYGWIN__) || defined(__INTERIX)

#include <sys/mman.h>

/* Cygwin is Linux-like, but not quite that Linux-like.  */
#define is_selinux_enabled() 0

#endif /* !defined(X86_WIN32) && !defined(X86_WIN64) */

#define is_emutramp_enabled() 0

/* Declare all functions defined in dlmalloc.c as static.  */
static void *dlmalloc(size_t);
static void dlfree(void*);
static void *dlcalloc(size_t, size_t) MAYBE_UNUSED;
static void *dlrealloc(void *, size_t) MAYBE_UNUSED;
static void *dlmemalign(size_t, size_t) MAYBE_UNUSED;
static void *dlvalloc(size_t) MAYBE_UNUSED;
static int dlmallopt(int, int) MAYBE_UNUSED;
static size_t dlmalloc_footprint(void) MAYBE_UNUSED;
static size_t dlmalloc_max_footprint(void) MAYBE_UNUSED;
static void** dlindependent_calloc(size_t, size_t, void**) MAYBE_UNUSED;
static void** dlindependent_comalloc(size_t, size_t*, void**) MAYBE_UNUSED;
static void *dlpvalloc(size_t) MAYBE_UNUSED;
static int dlmalloc_trim(size_t) MAYBE_UNUSED;
static size_t dlmalloc_usable_size(void*) MAYBE_UNUSED;
static void dlmalloc_stats(void) MAYBE_UNUSED;

#if !(defined(_WIN32) || defined(__OS2__)) || defined (__CYGWIN__) || defined(__INTERIX)
/* Use these for mmap and munmap within dlmalloc.c.  */
static void *dlmmap(void *, size_t, int, int, int, off_t);
static int dlmunmap(void *, size_t);
#endif /* !(defined(_WIN32) || defined(__OS2__)) || defined (__CYGWIN__) || defined(__INTERIX) */

#define mmap dlmmap
#define munmap dlmunmap

#include "dlmalloc.c"

void
ffi_deinit (void)
{
  msegmentptr sp;

  sp = &gm->seg;
  while (sp != NULL)
    {
      char *base = sp->base;
      size_t size = sp->size;
      flag_t flag = get_segment_flags (sp);

      sp = sp->next;

      if ((flag & IS_MMAPPED_BIT) && !(flag & EXTERN_BIT))
        {
          CALL_MUNMAP (base, size);
        }
    }

  (void) DESTROY_LOCK (&gm->mutex);
}

#undef mmap
#undef munmap

#if !(defined(_WIN32) || defined(__OS2__)) || defined (__CYGWIN__) || defined(__INTERIX)

/* A mutex used to synchronize access to *exec* variables in this file.  */
static pthread_mutex_t open_temp_exec_file_mutex = PTHREAD_MUTEX_INITIALIZER;

/* A file descriptor of a temporary file from which we'll map
   executable pages.  */
static int execfd = -1;

/* The amount of space already allocated from the temporary file.  */
static size_t execsize = 0;

#ifdef HAVE_MEMFD_CREATE
/* Open a temporary file name, and immediately unlink it.  */
static int
open_temp_exec_file_memfd (const char *name)
{
  int fd;
  fd = memfd_create (name, MFD_CLOEXEC);
  return fd;
}
#endif

/* Open a temporary file name, and immediately unlink it.  */
static int
open_temp_exec_file_name (char *name, int flags)
{
  int fd;

#ifdef HAVE_MKOSTEMP
  fd = mkostemp (name, flags);
#else
  fd = mkstemp (name);
#endif

  if (fd != -1)
    unlink (name);

  return fd;
}

/* Open a temporary file in the named directory.  */
static int
open_temp_exec_file_dir (const char *dir)
{
  static const char suffix[] = "/ffiXXXXXX";
  int lendir, flags;
  char *tempname;
#ifdef O_TMPFILE
  int fd;
#endif

#ifdef O_CLOEXEC
  flags = O_CLOEXEC;
#else
  flags = 0;
#endif

#ifdef O_TMPFILE
  fd = open (dir, flags | O_RDWR | O_EXCL | O_TMPFILE, 0700);
  /* If the running system does not support the O_TMPFILE flag then retry without it. */
  if (fd != -1 || (errno != EINVAL && errno != EISDIR && errno != EOPNOTSUPP)) {
    return fd;
  } else {
    errno = 0;
  }
#endif

  lendir = (int) strlen (dir);
  tempname = __builtin_alloca (lendir + sizeof (suffix));

  if (!tempname)
    return -1;

  memcpy (tempname, dir, lendir);
  memcpy (tempname + lendir, suffix, sizeof (suffix));

  return open_temp_exec_file_name (tempname, flags);
}

/* Open a temporary file in the directory in the named environment
   variable.  */
static int
open_temp_exec_file_env (const char *envvar)
{
  const char *value = getenv (envvar);

  if (!value)
    return -1;

  return open_temp_exec_file_dir (value);
}

#ifdef HAVE_MNTENT
/* Open a temporary file in an executable and writable mount point
   listed in the mounts file.  Subsequent calls with the same mounts
   keep searching for mount points in the same file.  Providing NULL
   as the mounts file closes the file.  */
static int
open_temp_exec_file_mnt (const char *mounts)
{
  static const char *last_mounts;
  static FILE *last_mntent;

  if (mounts != last_mounts)
    {
      if (last_mntent)
	endmntent (last_mntent);

      last_mounts = mounts;

      if (mounts)
	last_mntent = setmntent (mounts, "r");
      else
	last_mntent = NULL;
    }

  if (!last_mntent)
    return -1;

  for (;;)
    {
      int fd;
      struct mntent mnt;
      char buf[MAXPATHLEN * 3];

      if (getmntent_r (last_mntent, &mnt, buf, sizeof (buf)) == NULL)
	return -1;

      if (hasmntopt (&mnt, "ro")
	  || hasmntopt (&mnt, "noexec")
	  || access (mnt.mnt_dir, W_OK))
	continue;

      fd = open_temp_exec_file_dir (mnt.mnt_dir);

      if (fd != -1)
	return fd;
    }
}
#endif /* HAVE_MNTENT */

/* Instructions to look for a location to hold a temporary file that
   can be mapped in for execution.  */
static struct
{
  int (*func)(const char *);
  const char *arg;
  int repeat;
} open_temp_exec_file_opts[] = {
#ifdef HAVE_MEMFD_CREATE
  { open_temp_exec_file_memfd, "libffi", 0 },
#endif
  { open_temp_exec_file_env, "TMPDIR", 0 },
  { open_temp_exec_file_dir, "/tmp", 0 },
  { open_temp_exec_file_dir, "/var/tmp", 0 },
  { open_temp_exec_file_dir, "/dev/shm", 0 },
  { open_temp_exec_file_env, "HOME", 0 },
#ifdef HAVE_MNTENT
  { open_temp_exec_file_mnt, "/etc/mtab", 1 },
  { open_temp_exec_file_mnt, "/proc/mounts", 1 },
#endif /* HAVE_MNTENT */
};

/* Current index into open_temp_exec_file_opts.  */
static int open_temp_exec_file_opts_idx = 0;

/* Reset a current multi-call func, then advances to the next entry.
   If we're at the last, go back to the first and return nonzero,
   otherwise return zero.  */
static int
open_temp_exec_file_opts_next (void)
{
  if (open_temp_exec_file_opts[open_temp_exec_file_opts_idx].repeat)
    open_temp_exec_file_opts[open_temp_exec_file_opts_idx].func (NULL);

  open_temp_exec_file_opts_idx++;
  if (open_temp_exec_file_opts_idx
      == (sizeof (open_temp_exec_file_opts)
	  / sizeof (*open_temp_exec_file_opts)))
    {
      open_temp_exec_file_opts_idx = 0;
      return 1;
    }

  return 0;
}

/* Return a file descriptor of a temporary zero-sized file in a
   writable and executable filesystem.  */
static int
open_temp_exec_file (void)
{
  int fd;

  do
    {
      fd = open_temp_exec_file_opts[open_temp_exec_file_opts_idx].func
	(open_temp_exec_file_opts[open_temp_exec_file_opts_idx].arg);

      if (!open_temp_exec_file_opts[open_temp_exec_file_opts_idx].repeat
	  || fd == -1)
	{
	  if (open_temp_exec_file_opts_next ())
	    break;
	}
    }
  while (fd == -1);

  return fd;
}

/* We need to allocate space in a file that will be backing a writable
   mapping.  Several problems exist with the usual approaches:
   - fallocate() is Linux-only
   - posix_fallocate() is not available on all platforms
   - ftruncate() does not allocate space on filesystems with sparse files
   Failure to allocate the space will cause SIGBUS to be thrown when
   the mapping is subsequently written to.  */
static int
allocate_space (int fd, off_t offset, off_t len)
{
  static size_t page_size;

  /* Obtain system page size. */
  if (!page_size)
    page_size = sysconf(_SC_PAGESIZE);

  unsigned char buf[page_size];
  memset (buf, 0, page_size);

  while (len > 0)
    {
      off_t to_write = (len < page_size) ? len : page_size;
      if (write (fd, buf, to_write) < to_write)
        return -1;
      len -= to_write;
    }

  return 0;
}

/* Map in a chunk of memory from the temporary exec file into separate
   locations in the virtual memory address space, one writable and one
   executable.  Returns the address of the writable portion, after
   storing an offset to the corresponding executable portion at the
   last word of the requested chunk.  */
static void *
dlmmap_locked (void *start, size_t length, int prot, int flags, off_t offset)
{
  void *ptr;

  if (execfd == -1)
    {
      open_temp_exec_file_opts_idx = 0;
    retry_open:
      execfd = open_temp_exec_file ();
      if (execfd == -1)
	return MFAIL;
    }

  offset = execsize;

  if (allocate_space (execfd, offset, length))
    return MFAIL;

  flags &= ~(MAP_PRIVATE | MAP_ANONYMOUS);
  flags |= MAP_SHARED;

  ptr = mmap (NULL, length, (prot & ~PROT_WRITE) | PROT_EXEC,
	      flags, execfd, offset);
  if (ptr == MFAIL)
    {
      if (!offset)
	{
	  close (execfd);
	  goto retry_open;
	}
      if (ftruncate (execfd, offset) != 0)
      {
        /* Fixme : Error logs can be added here. Returning an error for
         * ftruncte() will not add any advantage as it is being
         * validating in the error case. */
      }

      return MFAIL;
    }
  else if (!offset
	   && open_temp_exec_file_opts[open_temp_exec_file_opts_idx].repeat)
    open_temp_exec_file_opts_next ();

  start = mmap (start, length, prot, flags, execfd, offset);

  if (start == MFAIL)
    {
      munmap (ptr, length);
      if (ftruncate (execfd, offset) != 0)
      {
        /* Fixme : Error logs can be added here. Returning an error for
         * ftruncte() will not add any advantage as it is being
         * validating in the error case. */
      }
      return start;
    }

  mmap_exec_offset ((char *)start, length) = (char*)ptr - (char*)start;

  execsize += length;

  mem_callbacks.on_allocate (ptr, length);
  mem_callbacks.on_allocate (start, length);

  return start;
}

/* Map in a writable and executable chunk of memory if possible.
   Failing that, fall back to dlmmap_locked.  */
static void *
dlmmap (void *start, size_t length, int prot,
	int flags, int fd, off_t offset)
{
  void *ptr;

  assert (start == NULL && length % malloc_getpagesize == 0
	  && prot == (PROT_READ | PROT_WRITE)
	  && flags == (MAP_PRIVATE | MAP_ANONYMOUS)
	  && fd == -1 && offset == 0);

  if (execfd == -1 && is_emutramp_enabled ())
    {
      ptr = mmap (start, length, prot & ~PROT_EXEC, flags, fd, offset);
      if (ptr != MFAIL)
        mem_callbacks.on_allocate (ptr, length);
      return ptr;
    }

  if (execfd == -1 && !is_selinux_enabled ())
    {
      ptr = mmap (start, length, prot | PROT_EXEC, flags, fd, offset);
      if (ptr != MFAIL)
        mem_callbacks.on_allocate (ptr, length);

      if (ptr != MFAIL || (errno != EPERM && errno != EACCES))
	/* Cool, no need to mess with separate segments.  */
	return ptr;

      /* If MREMAP_DUP is ever introduced and implemented, try mmap
	 with ((prot & ~PROT_WRITE) | PROT_EXEC) and mremap with
	 MREMAP_DUP and prot at this point.  */
    }

  if (execsize == 0 || execfd == -1)
    {
      pthread_mutex_lock (&open_temp_exec_file_mutex);
      ptr = dlmmap_locked (start, length, prot, flags, offset);
      pthread_mutex_unlock (&open_temp_exec_file_mutex);

      return ptr;
    }

  return dlmmap_locked (start, length, prot, flags, offset);
}

/* Release memory at the given address, as well as the corresponding
   executable page if it's separate.  */
static int
dlmunmap (void *start, size_t length)
{
  /* We don't bother decreasing execsize or truncating the file, since
     we can't quite tell whether we're unmapping the end of the file.
     We don't expect frequent deallocation anyway.  If we did, we
     could locate pages in the file by writing to the pages being
     deallocated and checking that the file contents change.
     Yuck.  */
  msegmentptr seg = segment_holding (gm, start);
  void *code;
  int ret;

  if (seg && (code = add_segment_exec_offset (start, seg)) != start)
    {
      ret = munmap (code, length);
      if (ret)
	return ret;
      else
        mem_callbacks.on_deallocate (code, length);
    }

  ret = munmap (start, length);
  if (ret == 0)
    mem_callbacks.on_deallocate (start, length);
  return ret;
}

#if FFI_CLOSURE_FREE_CODE
/* Return segment holding given code address.  */
static msegmentptr
segment_holding_code (mstate m, char* addr)
{
  msegmentptr sp = &m->seg;
  for (;;) {
    if (addr >= add_segment_exec_offset (sp->base, sp)
	&& addr < add_segment_exec_offset (sp->base, sp) + sp->size)
      return sp;
    if ((sp = sp->next) == 0)
      return 0;
  }
}
#endif

#endif /* !(defined(_WIN32) || defined(__OS2__)) || defined (__CYGWIN__) || defined(__INTERIX) */

/* Allocate a chunk of memory with the given size.  Returns a pointer
   to the writable address, and sets *CODE to the executable
   corresponding virtual address.  */
void *
ffi_closure_alloc (size_t size, void **code)
{
  void *ptr;

  if (!code)
    return NULL;

  ptr = FFI_CLOSURE_PTR (dlmalloc (size));

  if (ptr)
    {
      msegmentptr seg = segment_holding (gm, ptr);

      *code = add_segment_exec_offset (ptr, seg);
    }

  return ptr;
}

void *
ffi_data_to_code_pointer (void *data)
{
  msegmentptr seg = segment_holding (gm, data);
  /* We expect closures to be allocated with ffi_closure_alloc(), in
     which case seg will be non-NULL.  However, some users take on the
     burden of managing this memory themselves, in which case this
     we'll just return data. */
  if (seg)
    return add_segment_exec_offset (data, seg);
  else
    return data;
}

/* Release a chunk of memory allocated with ffi_closure_alloc.  If
   FFI_CLOSURE_FREE_CODE is nonzero, the given address can be the
   writable or the executable address given.  Otherwise, only the
   writable address can be provided here.  */
void
ffi_closure_free (void *ptr)
{
#if FFI_CLOSURE_FREE_CODE
  msegmentptr seg = segment_holding_code (gm, ptr);

  if (seg)
    ptr = sub_segment_exec_offset (ptr, seg);
#endif

  dlfree (FFI_RESTORE_PTR (ptr));
}

# else /* ! FFI_MMAP_EXEC_WRIT */

/* On many systems, memory returned by malloc is writable and
   executable, so just use it.  */

#include <stdlib.h>

void
ffi_deinit (void)
{
}

void *
ffi_closure_alloc (size_t size, void **code)
{
  if (!code)
    return NULL;

  return *code = FFI_CLOSURE_PTR (mem_callbacks.malloc (size));
}

void
ffi_closure_free (void *ptr)
{
  mem_callbacks.free (FFI_RESTORE_PTR (ptr));
}

void *
ffi_data_to_code_pointer (void *data)
{
  return data;
}

# endif /* ! FFI_MMAP_EXEC_WRIT */
#else

void
ffi_deinit (void)
{
}

#endif /* FFI_CLOSURES */

#endif /* NetBSD with PROT_MPROTECT */
