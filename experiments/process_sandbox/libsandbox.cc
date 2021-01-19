// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include <array>
#include <mutex>
#include <queue>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef __unix__
#  include <dlfcn.h>
#  include <err.h>
#  include <fcntl.h>
#  include <libgen.h>
#  include <pthread.h>
#  include <stdio.h>
#  include <sys/mman.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <sys/un.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  ifdef USE_CAPSICUM
#    include <sys/capsicum.h>
#  endif
#  ifdef USE_KQUEUE_PROCDESC
#    include <sys/event.h>
#    include <sys/procdesc.h>
#  endif
#  ifdef __linux__
#    include <bsd/unistd.h>
#    define MAP_ALIGNED(x) 0
#    define MAP_NOCORE 0
#  endif
#endif
#include "host_service_calls.h"
#include "sandbox.hh"

extern "C"
{
  /**
   * The `environ` symbol is exported by libc, but not exposed in any header.
   *
   * This should go away once we are constructing a properly sanitised
   * environment for the child.
   */
  extern char** environ;
}

namespace sandbox
{
  /**
   * Singleton class that handles pagemap updates from children.  This listens
   * on a socket for updates, validates that they correspond to the memory that
   * this child is responsible for, and if so updates both that child's shared
   * pagemap page and the parent process's pagemap.
   *
   * This class creates a new thread in the background that waits for pagemap
   * updates and processes them.
   */
  class MemoryServiceProvider
  {
    snmalloc::DefaultChunkMap<> pm;
    platform::Poller poller;
    /**
     * Add a new socket that we'll wait for.  This can be called from any
     * thread without synchronisation.
     */
    void register_fd(int socket_fd)
    {
      poller.add(socket_fd);
    }
    /**
     * Mutex that protects the `ranges` map.
     */
    std::mutex m;
    /**
     * Metadata about a sandbox for which we are updating the page map.
     */
    struct Sandbox
    {
      /**
       * The memory provider that owns the above range.
       */
      SharedMemoryProvider* memory_provider;
      /**
       * The shared pagemap page that we need to update on behalf of this
       * process.
       */
      uint8_t* shared_page = nullptr;
    };
    /**
     * A map from file descriptor over which we've received an update request
     * to the sandbox metadata.
     */
    std::unordered_map<int, Sandbox> ranges;
    /**
     * Run loop.  Wait for updates from the child.
     */
    void run()
    {
      int fd;
      bool eof;
      while (poller.poll(fd, eof))
      {
        // If a child's socket closed, unmap its shared page and delete the
        // metadata that we have associated with it.
        if (eof)
        {
          std::lock_guard g(m);
          auto r = ranges.find(fd);
          ranges.erase(r);
          close(fd);
          continue;
        }
        HostServiceRequest rpc;
        if (
          read(fd, static_cast<void*>(&rpc), sizeof(HostServiceRequest)) !=
          sizeof(HostServiceRequest))
        {
          err(1, "Read from host service pipe %d failed", fd);
          // FIXME: We should kill the sandbox at this point.  It is doing
          // something bad.  For now, we kill the host process, which is safe
          // but slightly misses the point of fault isolation.
        }
        HostServiceResponse reply{1, 0};
        Sandbox s;
        {
          decltype(ranges)::iterator r;
          std::lock_guard g(m);
          r = ranges.find(fd);
          if (r == ranges.end())
          {
            continue;
          }
          s = r->second;
        }
        auto is_large_sizeclass = [](auto large_size) {
          return (large_size < snmalloc::NUM_LARGE_CLASSES);
        };
        // No default so we get range checking.  Fallthrough returns the error
        // result.
        switch (rpc.kind)
        {
          case MemoryProviderPushLargeStack:
          {
            // This may truncate, but only if the sandbox is doing
            // something bad.  We'll catch that on the range check (or get
            // some in-sandbox corruption that the sandbox could do
            // anyway), so don't bother checking it now.
            void* base = reinterpret_cast<void*>(rpc.arg0);
            uint8_t large_size = static_cast<uint8_t>(rpc.arg1);
            if (
              !is_large_sizeclass(large_size) ||
              !s.memory_provider->contains(
                base, snmalloc::large_sizeclass_to_size(large_size)))
            {
              break;
            }
            s.memory_provider->push_large_stack(
              static_cast<snmalloc::Largeslab*>(base), large_size);
            reply = {0, 0};
            break;
          }
          case MemoryProviderPopLargeStack:
          {
            uint8_t large_size = static_cast<uint8_t>(rpc.arg1);
            if (!is_large_sizeclass(large_size))
            {
              break;
            }
            reply = {0,
                     reinterpret_cast<uintptr_t>(
                       s.memory_provider->pop_large_stack(large_size))};
            break;
          }
          case MemoryProviderReserve:
          {
            uint8_t large_size = static_cast<uint8_t>(rpc.arg1);
            if (!is_large_sizeclass(large_size))
            {
              break;
            }
            reply = {0,
                     reinterpret_cast<uintptr_t>(
                       s.memory_provider->template reserve<true>(large_size))};
            break;
          }
          case ChunkMapSet:
          case ChunkMapSetRange:
          case ChunkMapClearRange:
          {
            reply.error = !validate_and_insert(s, rpc);
            break;
          }
        }
        write(fd, static_cast<void*>(&reply), sizeof(HostServiceResponse));
      }
      err(1, "Waiting for pagetable updates failed");
    }
    /**
     * Validate a request from the sandbox to update a pagemap and insert it if
     * allowed.  The `sender` parameter is the file descriptor over which the
     * message was sent. The `position` parameter is the address of the memory
     * for which the corresponding pagemap entry is to be updated.  For the
     * update to succeed, this must be within the range owned by the sandbox
     * identified by the sending socket.  The last two parameters indicate
     * whether this is a large allocation (one that spans multiple pagemap
     * entries) and the value.  If `isBig` is 0, then this is a simple update
     * of a single pagemap entry (typically a slab), specified in the `value`
     * parameter.  Otherwise, `value` is the base-2 logarithm of the size,
     * which is either being set or cleared (`isBig` values of 1 and 2,
     * respectively).
     */
    bool validate_and_insert(Sandbox& s, HostServiceRequest rpc)
    {
      void* address = reinterpret_cast<void*>(rpc.arg0);
      snmalloc::ChunkmapPagemap& cpm = snmalloc::GlobalPagemap::pagemap();
      size_t index = cpm.index_for_address(rpc.arg0);
      size_t entries = 1;
      bool safe = true;
      auto check_large_update = [&]() {
        size_t alloc_size = (1ULL << rpc.arg1);
        entries = alloc_size / snmalloc::SUPERSLAB_SIZE;
        return s.memory_provider->contains(address, alloc_size);
      };
      switch (rpc.kind)
      {
        default:
          // Should be unreachable
          assert(0);
          break;
        case ChunkMapSet:
          if ((safe = s.memory_provider->contains(
                 address, snmalloc::SUPERSLAB_SIZE)))
          {
            cpm.set(rpc.arg0, rpc.arg1);
          }
          break;
        case ChunkMapSetRange:
          if ((safe = check_large_update()))
          {
            pm.set_large_size(address, 1ULL << rpc.arg1);
          }
          break;
        case ChunkMapClearRange:
          if ((safe = check_large_update()))
          {
            pm.clear_large_size(address, 1ULL << rpc.arg1);
          }
      }
      if (safe)
      {
        for (size_t i = 0; i < entries; i++)
        {
          s.shared_page[index + i] =
            pm.get(pointer_offset(address, i * snmalloc::SUPERSLAB_SIZE));
        }
      }
      return safe;
    }

  public:
    /**
     * Constructor.  Spawns a background thread to run and process updates.
     */
    MemoryServiceProvider()
    {
      std::thread t([&]() { run(); });
      t.detach();
    }
    /**
     * Notify this class that a sandbox exists.  The `start` and `end`
     * parameters indicate the address range assigned to this sandbox.
     * `socket_fd` provides the file descriptor for the socket over which the
     * sandbox will send update requests.  `pagemap_fd` is the shared pagemap
     * page.
     */
    void add_range(
      SharedMemoryProvider* memory_provider, int socket_fd, platform::SharedMemoryMap &page)
    {
      {
        std::lock_guard g(m);
        ranges[socket_fd] = {memory_provider, static_cast<uint8_t*>(page.get_base())};
      }
      register_fd(socket_fd);
    }
  };
  /**
   * Return a singleton instance of the pagemap owner.
   */
  MemoryServiceProvider& pagemap_owner()
  {
    // Leaks.  No need to run the destructor!
    static MemoryServiceProvider* p = new MemoryServiceProvider();
    return *p;
  }

  /**
   * Adaptor for allocators in the shared region to update the pagemap.
   * These treat the global pagemap in the process as canonical but also
   * update the pagemap in the child whenever the parent allocates within the
   * shared region.
   */
  struct SharedPagemapAdaptor
  {
    /**
     * Interface to the global pagemap.  Used to update the global pagemap and
     * to query values to propagate to the child process.
     */
    snmalloc::DefaultChunkMap<> global_pagemap;
    /**
     * The page in the child process that will be mapped into its pagemap.  Any
     * slab allocations by the parent must be propagated into this page.
     */
    uint8_t* shared_page;

    /**
     * Constructor.  Takes a shared pagemap page that this adaptor will update
     * in addition to updating the global pagemap.
     */
    SharedPagemapAdaptor(uint8_t* p) : shared_page(p) {}

    /**
     * Update the child, propagating `entries` entries from the global pagemap
     * into the shared pagemap region.
     */
    void update_child(uintptr_t p, size_t entries = 1)
    {
      snmalloc::ChunkmapPagemap& cpm = snmalloc::GlobalPagemap::pagemap();
      size_t index = cpm.index_for_address(p);
      for (size_t i = 0; i < entries; i++)
      {
        shared_page[index + i] =
          global_pagemap.get(p + (i * snmalloc::SUPERSLAB_SIZE));
      }
    }
    /**
     * Accessor.  We treat the global pagemap as canonical, so only look values
     * up here.
     */
    uint8_t get(uintptr_t p)
    {
      return global_pagemap.get(p);
    }
    /**
     * Set a superslab entry in the pagemap.  Inserts it into the global
     * pagemap and then propagates to the child.
     */
    void set_slab(snmalloc::Superslab* slab)
    {
      global_pagemap.set_slab(slab);
      update_child(reinterpret_cast<uintptr_t>(slab));
    }
    /**
     * Clear a superslab entry in the pagemap.  Removes it from the global
     * pagemap and then propagates to the child.
     */
    void clear_slab(snmalloc::Superslab* slab)
    {
      global_pagemap.clear_slab(slab);
      update_child(reinterpret_cast<uintptr_t>(slab));
    }
    /**
     * Clear a medium slab entry in the pagemap.  Removes it from the global
     * pagemap and then propagates to the child.
     */
    void clear_slab(snmalloc::Mediumslab* slab)
    {
      global_pagemap.clear_slab(slab);
      update_child(reinterpret_cast<uintptr_t>(slab));
    }
    /**
     * Set a medium slab entry in the pagemap.  Inserts it into the global
     * pagemap and then propagates to the child.
     */
    void set_slab(snmalloc::Mediumslab* slab)
    {
      global_pagemap.set_slab(slab);
      update_child(reinterpret_cast<uintptr_t>(slab));
    }
    /**
     * Set a large entry in the pagemap.  Inserts it into the global
     * pagemap and then propagates to the child.
     */
    void set_large_size(void* p, size_t size)
    {
      global_pagemap.set_large_size(p, size);
      size_t entries = size / snmalloc::SUPERSLAB_SIZE;
      update_child(reinterpret_cast<uintptr_t>(p), entries);
    }
    /**
     * Clear a large entry in the pagemap.  Removes it from the global
     * pagemap and then propagates to the child.
     */
    void clear_large_size(void* p, size_t size)
    {
      global_pagemap.clear_large_size(p, size);
      size_t entries = size / snmalloc::SUPERSLAB_SIZE;
      update_child(reinterpret_cast<uintptr_t>(p), entries);
    }
  };

  /**
   * Class representing a view of a shared memory region.  This provides both
   * the parent and child views of the region.
   */
  struct SharedMemoryRegion
  {
    // FIXME: The parent process can currently blindly follow pointers in these
    // regions.  We should explicitly mask all pointers against the size of the
    // allocation when we use them from outside.
    /**
     * The start of the sandbox region.  Note: This is writeable from within
     * the sandbox and should not be trusted outside.
     */
    void* start;

    /**
     * The end of the sandbox region.  Note: This is writeable from within
     * the sandbox and should not be trusted outside.
     */
    void* end;

    /**
     * A flag indicating that the parent has instructed the sandbox to exit.
     */
    std::atomic<bool> should_exit = false;
    /**
     * The index of the function currently being called.  This interface is not
     * currently reentrant.
     */
    int function_index;
    /**
     * A pointer to the tuple (in the shared memory range) that contains the
     * argument frame provided by the sandbox caller.
     */
    void* msg_buffer = nullptr;
    /**
     * The message queue for the parent's allocator.  This is stored in the
     * shared region because the child must be able to free memory allocated by
     * the parent.
     */
    snmalloc::RemoteAllocator allocator_state;
#ifdef __unix__
    /**
     * Mutex used to protect `cv`.
     */
    pthread_mutex_t mutex;
    /**
     * The condition variable that the child sleeps on when waiting for
     * messages from the parent.
     */
    pthread_cond_t cv;
    /**
     * Flag indicating whether the child is executing.  Set on startup and
     */
    std::atomic<bool> is_child_executing = false;
#endif
    /**
     * Waits until the `is_child_executing` flag is in the `expected` state.
     * This is used to wait for the child to start and to stop.
     */
    void wait(bool expected);
    /**
     * Wait until the `is_child_executing` flag is in the `expected` state.
     * Returns true if the condition was met or false if the timeout was
     * exceeded before the child entered the desired state.
     */
    bool wait(bool expected, struct timespec timeout);
    /**
     * Update the `is_child_executing` flag and wake up any waiters.  Note that
     * the `wait` functions will only unblock if `is_child_executing` is
     * modified using this function.
     */
    void signal(bool new_state);
    /**
     * Constructor.  Initialises the mutex and condition variables.
     */
    SharedMemoryRegion();
    /**
     * Tear down the parent-owned contents of this shared memory region.
     */
    void destroy();
  };

#ifdef __unix__
  void SharedMemoryRegion::wait(bool expected)
  {
    pthread_mutex_lock(&mutex);
    while (expected != is_child_executing)
    {
      pthread_cond_wait(&cv, &mutex);
    }
    pthread_mutex_unlock(&mutex);
  }

  bool SharedMemoryRegion::wait(bool expected, struct timespec timeout)
  {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long nsec;
    time_t carry = __builtin_add_overflow(now.tv_nsec, timeout.tv_nsec, &nsec);
    timeout.tv_nsec = nsec;
    timeout.tv_sec += now.tv_sec + carry;
    pthread_mutex_lock(&mutex);
    pthread_cond_timedwait(&cv, &mutex, &timeout);
    bool ret = expected == is_child_executing;
    pthread_mutex_unlock(&mutex);
    return ret;
  }

  void SharedMemoryRegion::signal(bool new_state)
  {
    pthread_mutex_lock(&mutex);
    is_child_executing = new_state;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mutex);
  }

  SharedMemoryRegion::SharedMemoryRegion()
  {
    pthread_mutexattr_t mattrs;
    pthread_mutexattr_init(&mattrs);
    pthread_mutexattr_setpshared(&mattrs, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&mutex, &mattrs);
    pthread_condattr_t cvattrs;
    pthread_condattr_init(&cvattrs);
    pthread_condattr_setpshared(&cvattrs, PTHREAD_PROCESS_SHARED);
    pthread_condattr_setclock(&cvattrs, CLOCK_MONOTONIC);
    pthread_cond_init(&cv, &cvattrs);
  }

  void SharedMemoryRegion::destroy()
  {
    pthread_mutex_destroy(&mutex);
    pthread_cond_destroy(&cv);
  }
#else
#  error Missing implementation of SharedMemoryRegion
#endif

#ifdef __unix__
  SandboxedLibrary::~SandboxedLibrary()
  {
    wait_for_child_exit();
    shared_mem->destroy();
    close(socket_fd);
#  ifdef USE_KQUEUE_PROCDESC
    close(kq);
#  endif
  }

  /**
   * The numbers for file descriptors passed into the child.  These must match
   * between libsandbox and the library runner child process.
   */
  enum SandboxFileDescriptors
  {
    /**
     * The file descriptor used for the shared memory object that contains the
     * shared heap.
     */
    SharedMemRegion = 3,
    /**
     * The file descriptor for the shared memory object that contains the
     * shared pagemap page.  This is mapped read-only in the child and updated
     * in the parent.
     */
    PageMapPage,
    /**
     * The file descriptor for the socket used to pass file descriptors into the
     * child.
     */
    FDSocket,
    /**
     * The file descriptor used for the main library.  This is passed to
     * `fdlopen` in the child.
     */
    MainLibrary,
    /**
     * The file descriptor for the pipe used to send pagemap updates to the
     * parent process.
     */
    PageMapUpdates,
    /**
     * The first file descriptor number used for directory descriptors of
     * library directories.  These are used by rtld in the child to locate
     * libraries that the library identified by `MainLibrary`depends on.
     */
    OtherLibraries
  };

  void SandboxedLibrary::start_child(
    const char* library_name,
    const char* librunnerpath,
    const void* sharedmem_addr,
    int pagemap_mem,
    int malloc_rpc_socket,
    int fd_socket)
  {
    // The library load paths.  We're going to pass all of these to the
    // child as open directory descriptors for the run-time linker to use.
    std::array<const char*, 3> libdirs = {"/lib", "/usr/lib", "/usr/local/lib"};
    // The file descriptors for the directories in libdirs
    std::array<int, libdirs.size()> libdirfds;
    // The last file descriptor that we're going to use.  The `move_fd`
    // lambda will copy all file descriptors above this line so they can then
    // be copied into their desired location.
    static const int last_fd = OtherLibraries + libdirs.size();
    auto move_fd = [](int x) {
      assert(x >= 0);
      while (x < last_fd)
      {
        x = dup(x);
      }
      return x;
    };
    // Move all of the file descriptors that we're going to use out of the
    // region that we're going to populate.
    int shm_fd = move_fd(shm.get_handle().fd);
    pagemap_mem = move_fd(pagemap_mem);
    fd_socket = move_fd(fd_socket);
    malloc_rpc_socket = move_fd(malloc_rpc_socket);
    // Open the library binary.  If this fails, kill the child process.  Note
    // that we do this *before* dropping privilege - we don't have to give
    // the child the right to look in the directory that contains this
    // binary.
    int library = open(library_name, O_RDONLY);
    if (library < 0)
    {
      _exit(-1);
    }
    library = move_fd(library);
    for (size_t i = 0; i < libdirs.size(); i++)
    {
      libdirfds.at(i) = move_fd(open(libdirs.at(i), O_DIRECTORY));
    }
    // The child process expects to find these in fixed locations.
    shm_fd = dup2(shm_fd, SharedMemRegion);
    pagemap_mem = dup2(pagemap_mem, PageMapPage);
    fd_socket = dup2(fd_socket, FDSocket);
    assert(library);
    library = dup2(library, MainLibrary);
    assert(library == MainLibrary);
    malloc_rpc_socket = dup2(malloc_rpc_socket, PageMapUpdates);
    // These are passed in by environment variable, so we don't need to put
    // them in a fixed place, just after all of the others.
    int rtldfd = OtherLibraries;
    for (auto& libfd : libdirfds)
    {
      libfd = dup2(libfd, rtldfd++);
    }
#  ifdef USE_CAPSICUM
    // If we're compiling with Capsicum support, then restrict the permissions
    // on all of the file descriptors that are available to untrusted code.
    auto limit_fd = [&](int fd, auto... permissions) {
      cap_rights_t rights;
      if (cap_rights_limit(fd, cap_rights_init(&rights, permissions...)) != 0)
      {
        err(1, "Failed to limit rights on file descriptor %d", fd);
      }
    };
    // Standard in is read only
    limit_fd(STDIN_FILENO, CAP_READ);
    // Standard out and error are write only
    limit_fd(STDOUT_FILENO, CAP_WRITE);
    limit_fd(STDERR_FILENO, CAP_WRITE);
    // The socket is used with a call-return protocol for requesting services
    // for malloc.
    limit_fd(malloc_rpc_socket, CAP_WRITE, CAP_READ);
    // The shared heap can be mapped read-write, but can't be truncated.
    limit_fd(shm_fd, CAP_MMAP_RW);
    limit_fd(pagemap_mem, CAP_MMAP_R);
    // The library must be parseable and mappable by rtld
    limit_fd(library, CAP_READ, CAP_FSTAT, CAP_SEEK, CAP_MMAP_RX);
    // The libraries implicitly opened from the library directories inherit
    // the permissions from the parent directory descriptors.  These need the
    // permissions required to map a library and also the permissions
    // required to search the directory to find the relevant libraries.
    for (auto libfd : libdirfds)
    {
      limit_fd(libfd, CAP_READ, CAP_FSTAT, CAP_LOOKUP, CAP_MMAP_RX);
    }
#  endif
    closefrom(last_fd);
    // Prepare the arguments to main.  These are going to be the binary name,
    // the address of the shared memory region, the length of the shared
    // memory region, and a null terminator.  We have to pass the two
    // addresses as strings because the kernel will assume that all arguments
    // to main are null-terminated strings and will copy them into the
    // process initialisation structure.
    // Note that we create these strings on the stack, rather than calling
    // asprintf, because (if we used vfork) we're still in the same address
    // space as the parent, so if we allocate memory here then it will leak in
    // the parent.
    char* args[2];
    args[0] = (char*)"library_runner";
    args[1] = nullptr;
    char location[52];
    size_t loc_len = snprintf(
      location,
      sizeof(location),
      "SANDBOX_LOCATION=%zx:%zx",
      (size_t)sharedmem_addr,
      (size_t)shm.get_size());
    assert(loc_len < sizeof(location));
    static_assert(
      OtherLibraries == 8, "First entry in LD_LIBRARY_PATH_FDS is incorrect");
    static_assert(
      libdirfds.size() == 3,
      "Number of entries in LD_LIBRARY_PATH_FDS is incorrect");
    //const char* const env[] = {"LD_LIBRARY_PATH_FDS=8:9:10", location, nullptr};
    const char* const env[] = {location, nullptr};
    execve(librunnerpath, args, const_cast<char* const*>(env));
    // Should be unreachable, but just in case we failed to exec, don't return
    // from here (returning from a vfork context is very bad!).
    _exit(EXIT_FAILURE);
  }

  SandboxedLibrary::SandboxedLibrary(const char* library_name, size_t size)
  : shm(snmalloc::bits::next_pow2_bits(size << 30)),
    shared_pagemap(snmalloc::bits::next_pow2_bits(snmalloc::OS_PAGE_SIZE)),
    memory_provider(
      pointer_offset(shm.get_base(), sizeof(SharedMemoryRegion)),
      shm.get_size() - sizeof(SharedMemoryRegion))
  {
    void* shm_base = shm.get_base();
    // Allocate the shared memory region and set its memory provider to use all
    // of the space after the end of the header for subsequent allocations.
    shared_mem = new (shm_base) SharedMemoryRegion();
    shared_mem->start = shm_base;
    shared_mem->end = pointer_offset(shm.get_base(), shm.get_size());

    // Create a pair of sockets that we can use to
    int malloc_rpc_sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, malloc_rpc_sockets))
    {
      err(1, "Failed to create socket pair");
    }
    pagemap_owner().add_range(
      &memory_provider, malloc_rpc_sockets[0], shared_pagemap);
    // Construct a UNIX domain socket.  This will eventually be used to send
    // file descriptors from the parent to the child, but isn't yet.
    int socks[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, socks);
    int pid;
    std::string path = ".";
    std::string lib;
    // Use dladdr to find the path of the libsandbox shared library.  For now,
    // we assume that the library runner is in the same place and so is the
    // library that we're going to open.  Eventually we should look for
    // library_runner somewhere else (e.g. ../libexec) and search
    // LD_LIBRARY_PATH for the library that we're going to open.
    Dl_info info;
    static char x;
    if (dladdr(&x, &info))
    {
      char* libpath = ::strdup(info.dli_fname);
      path = dirname(libpath);
      ::free(libpath);
    }
    if (library_name[0] == '/')
    {
      lib = library_name;
    }
    else
    {
      lib = path;
      lib += '/';
      lib += library_name;
    }
    library_name = lib.c_str();
    path += "/library_runner";
    const char* librunnerpath = path.c_str();
    // We shouldn't do anything that modifies the heap (or reads the heap in
    // a way that is not concurrency safe) between vfork and exec.
    child_proc = -1;
#  ifdef USE_KQUEUE_PROCDESC
    pid = pdfork(&child_proc, PD_DAEMON | PD_CLOEXEC);
#  else
    child_proc = pid = vfork();
    assert(child_proc != -1);
#  endif
    if (pid == 0)
    {
      // In the child process.
      start_child(
        library_name,
        librunnerpath,
        shm_base,
        shared_pagemap.get_handle().fd,
        malloc_rpc_sockets[1],
        socks[1]);
    }
    // Only reachable in the parent process
#  ifdef USE_KQUEUE_PROCDESC
    // If we're using kqueue to monitor for child failure, construct a kqueue
    // now and add this as the event that we'll monitor.  Otherwise, we'll use
    // waitpid with the pid and don't need to maintain any in-kernel state.
    kq = kqueue();
    struct kevent event;
    EV_SET(&event, child_proc, EVFILT_PROCDESC, EV_ADD, NOTE_EXIT, 0, nullptr);
    if (kevent(kq, &event, 1, nullptr, 0, nullptr) == -1)
    {
      err(1, "Setting up kqueue");
    }
#  endif
    // Close all of the file descriptors that only the child should have.
    close(socks[1]);
    close(malloc_rpc_sockets[1]);
    socket_fd = socks[0];
    // Allocate an allocator in the shared memory region.
    allocator = new SharedAlloc(
      memory_provider,
      SharedPagemapAdaptor(static_cast<uint8_t*>(shared_pagemap.get_base())),
      &shared_mem->allocator_state);
  }
  void SandboxedLibrary::send(int idx, void* ptr)
  {
    shared_mem->function_index = idx;
    shared_mem->msg_buffer = ptr;
    shared_mem->signal(true);
    // Wait for a second, see if the child has exited, if it's still going, try
    // again.
    // FIXME: We should probably allow the user to specify a maxmimum execution
    // time for all calls and kill the sandbox and raise an exception if it's
    // taking too long.
    while (!shared_mem->wait(false, {0, 100000}))
    {
      if (has_child_exited())
      {
        throw std::runtime_error("Sandboxed library terminated abnormally");
      }
    }
  }
#  ifndef USE_KQUEUE_PROCDESC
  namespace
  {
    std::pair<pid_t, int> waitpid(pid_t child_proc, int options)
    {
      pid_t ret;
      int status;
      bool retry = false;
      do
      {
        ret = ::waitpid(child_proc, &status, options);
        retry = (ret == -1) && (errno == EINTR);
      } while (retry);
      return {ret, status};
    }
  }
#  endif
  bool SandboxedLibrary::has_child_exited()
  {
#  ifdef USE_KQUEUE_PROCDESC
    // If we're using kqueue and process descriptors then we
    struct kevent event;
    shared_mem->signal(true);
    struct timespec timeout = {0, 0};
    int ret = kevent(kq, nullptr, 0, &event, 1, &timeout);
    if (ret == -1)
    {
      err(1, "Waiting for child failed");
    }
    if (ret == 1)
    {
      child_status = event.data;
      child_exited = true;
    }
    return (ret == 1);
#  else
    auto [ret, status] = waitpid(child_proc, WNOHANG);
    if (ret == -1)
    {
      err(1, "Waiting for child failed");
    }
    if (ret == child_proc)
    {
      child_status = WEXITSTATUS(status);
      child_exited = true;
      return true;
    }
    return false;
#  endif
  }
  int SandboxedLibrary::wait_for_child_exit()
  {
    if (child_exited)
    {
      return child_status;
    }
    shared_mem->should_exit = true;
    shared_mem->signal(true);
#  ifdef USE_KQUEUE_PROCDESC
    struct kevent event;
    // FIXME: Timeout and increase the aggression with which we kill the child
    // process (SIGTERM, SIGKILL)
    if (kevent(kq, nullptr, 0, &event, 1, nullptr) == -1)
    {
      err(1, "Waiting for child failed");
      abort();
    }
    return event.data;
#  else
    // FIXME: Timeout and increase the aggression with which we kill the child
    // process (SIGTERM, SIGKILL)
    auto [ret, status] = waitpid(child_proc, 0);
    if (ret == -1)
    {
      err(1, "Waiting for child failed");
      abort();
    }
    if (ret == child_proc && (WIFEXITED(status) || WIFSIGNALED(status)))
    {
      child_status = WEXITSTATUS(status);
      child_exited = true;
      return true;
    }
    return false;
#  endif
  }
#else
#  error Missing implementation of SandboxedLibrary
#endif

  void* SandboxedLibrary::alloc_in_sandbox(size_t bytes, size_t count)
  {
    bool overflow = false;
    size_t sz = snmalloc::bits::umul(bytes, count, overflow);
    if (overflow)
    {
      return nullptr;
    }
    return allocator->alloc(sz);
  }
  void SandboxedLibrary::dealloc_in_sandbox(void* ptr)
  {
    allocator->dealloc(ptr);
  }
}
