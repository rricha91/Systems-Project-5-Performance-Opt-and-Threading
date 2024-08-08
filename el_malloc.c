

// el_malloc.c: implementation of explicit list malloc functions.

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "el_malloc.h"

////////////////////////////////////////////////////////////////////////////////
// Global control functions

// Global control variable for the allocator. Must be initialized in
// el_init().
el_ctl_t *el_ctl = NULL;

// Create an initial block of memory for the heap using
// mmap(). Initialize the el_ctl data structure to point at this
// block. The initializ size/position of the heap for the memory map
// are given in the symbols EL_HEAP_INITIAL_SIZE and
// EL_HEAP_START_ADDRESS.  Initialize the lists in el_ctl to contain a
// single large block of available memory and no used blocks of
// memory.
int el_init(){
  el_ctl =
    mmap(EL_CTL_START_ADDRESS,
         EL_PAGE_BYTES,
         PROT_READ | PROT_WRITE,
         MAP_PRIVATE | MAP_ANONYMOUS,
         -1, 0);
  assert(el_ctl == EL_CTL_START_ADDRESS);

  void *heap = 
    mmap(EL_HEAP_START_ADDRESS,
         EL_HEAP_INITIAL_SIZE,
         PROT_READ | PROT_WRITE,
         MAP_PRIVATE | MAP_ANONYMOUS,
         -1, 0);
  assert(heap == EL_HEAP_START_ADDRESS);

  el_ctl->heap_bytes = EL_HEAP_INITIAL_SIZE; // make the heap as big as possible to begin with
  el_ctl->heap_start = heap;                 // set addresses of start and end of heap
  el_ctl->heap_end   = PTR_PLUS_BYTES(heap,el_ctl->heap_bytes);

  if(el_ctl->heap_bytes < EL_BLOCK_OVERHEAD){
    fprintf(stderr,"el_init: heap size %ld to small for a block overhead %ld\n",
            el_ctl->heap_bytes,EL_BLOCK_OVERHEAD);
    return 1;
  }
 
  el_init_blocklist(&el_ctl->avail_actual);
  el_init_blocklist(&el_ctl->used_actual);
  el_ctl->avail = &el_ctl->avail_actual;
  el_ctl->used  = &el_ctl->used_actual;

  // establish the first available block by filling in size in
  // block/foot and null links in head
  size_t size = el_ctl->heap_bytes - EL_BLOCK_OVERHEAD;
  el_blockhead_t *ablock = el_ctl->heap_start;
  ablock->size = size;
  ablock->state = EL_AVAILABLE;
  el_blockfoot_t *afoot = el_get_footer(ablock);
  afoot->size = size;

  // Add initial block to availble list; avoid use of list add
  // functions in case those are buggy which will screw up the heap
  // initialization
  ablock->prev = el_ctl->avail->beg;
  ablock->next = el_ctl->avail->beg->next;
  ablock->prev->next = ablock;
  ablock->next->prev = ablock;
  el_ctl->avail->length++;
  el_ctl->avail->bytes += (ablock->size + EL_BLOCK_OVERHEAD);

  return 0;
}

// Clean up the heap area associated with the system which unmaps all
// pages associated with the heap.
void el_cleanup(){
  munmap(el_ctl->heap_start, el_ctl->heap_bytes);
  munmap(el_ctl, EL_PAGE_BYTES);
}

////////////////////////////////////////////////////////////////////////////////
// Pointer arithmetic functions to access adjacent headers/footers

// Compute the address of the foot for the given head which is at a
// higher address than the head.
el_blockfoot_t *el_get_footer(el_blockhead_t *head){
  size_t size = head->size;
  el_blockfoot_t *foot = PTR_PLUS_BYTES(head, sizeof(el_blockhead_t) + size);
  return foot;
}

// REQUIRED
// Compute the address of the head for the given foot which is at a
// lower address than the foot.
el_blockhead_t *el_get_header(el_blockfoot_t *foot){
  size_t size = foot->size;
  el_blockhead_t *head = PTR_MINUS_BYTES(foot, sizeof(el_blockhead_t) + size);
  return head;
}

// Return a pointer to the block that is one block higher in memory
// from the given block.  This should be the size of the block plus
// the EL_BLOCK_OVERHEAD which is the space occupied by the header and
// footer. Returns NULL if the block above would be off the heap.
// DOES NOT follow next pointer, looks in adjacent memory.
el_blockhead_t *el_block_above(el_blockhead_t *block){
  el_blockhead_t *higher =
    PTR_PLUS_BYTES(block, block->size + EL_BLOCK_OVERHEAD);
  if((void *) higher >= (void*) el_ctl->heap_end){
    return NULL;
  }
  else{
    return higher;
  }
}

// REQUIRED
// Return a pointer to the block that is one block lower in memory
// from the given block.  Uses the size of the preceding block found
// in its foot. DOES NOT follow block->next pointer, looks in adjacent
// memory. Returns NULL if the block below would be outside the heap.
// 
// WARNING: This function must perform slightly different arithmetic
// than el_block_above(). Take care when implementing it.
el_blockhead_t *el_block_below(el_blockhead_t *block){
  // Get the spot in memory where the foot of the preceding block should be
  el_blockfoot_t *foot = PTR_MINUS_BYTES(block,sizeof(el_blockfoot_t));

  // Check if the foot is below the start of the heep. If it is, that is not part of our heep! Return NULL
  if ((void *) foot <= el_ctl->heap_start) return NULL;

  // Otherwise get the address for the start of the lower block using the current block's size + the overhead
 else {
    el_blockhead_t *lower = PTR_MINUS_BYTES(block, foot->size + EL_BLOCK_OVERHEAD);
    return lower;
 }
}

////////////////////////////////////////////////////////////////////////////////
// Block list operations

// Print an entire blocklist. The format appears as follows.
//
// {length:   2  bytes:  3400}
//   [  0] head @ 0x600000000000 {state: a  size:   128}
//   [  1] head @ 0x600000000360 {state: a  size:  3192}
//
// Note that the '@' column uses the actual address of items which
// relies on a consistent mmap() starting point for the heap.
void el_print_blocklist(el_blocklist_t *list){
  printf("{length: %3lu  bytes: %5lu}\n", list->length,list->bytes);
  el_blockhead_t *block = list->beg;
  for(int i=0; i<list->length; i++){
    printf("  ");
    block = block->next;
    printf("[%3d] head @ %p ", i, block);
    printf("{state: %c  size: %5lu}\n", block->state,block->size);
  }
}


// Print a single block during a sequential walk through the heap
void el_print_block(el_blockhead_t *block){
  el_blockfoot_t *foot = el_get_footer(block);
  printf("%p\n", block);
  printf("  state:      %c\n", block->state);
  printf("  size:       %lu (total: 0x%lx)\n", block->size, block->size+EL_BLOCK_OVERHEAD);
  printf("  prev:       %p\n", block->prev);
  printf("  next:       %p\n", block->next);
  printf("  user:       %p\n", PTR_PLUS_BYTES(block,sizeof(el_blockhead_t)));
  printf("  foot:       %p\n", foot);
  printf("  foot->size: %lu\n", foot->size);
}

// Print out stats on the heap for use in debugging. Shows the
// available and used list along with a linear walk through the heap
// blocks.
void el_print_stats(){
  printf("HEAP STATS (overhead per node: %lu)\n",EL_BLOCK_OVERHEAD);
  printf("heap_start:  %p\n",el_ctl->heap_start); 
  printf("heap_end:    %p\n",el_ctl->heap_end); 
  printf("total_bytes: %lu\n",el_ctl->heap_bytes);
  printf("AVAILABLE LIST: ");
  el_print_blocklist(el_ctl->avail);
  printf("USED LIST: ");
  el_print_blocklist(el_ctl->used);
  printf("HEAP BLOCKS:\n");
  int i = 0;
  el_blockhead_t *cur = el_ctl->heap_start;
  while(cur != NULL){
    printf("[%3d] @ ",i);
    el_print_block(cur);
    cur = el_block_above(cur);
    i++;
  }
}

// Initialize the specified list to be empty. Sets the beg/end
// pointers to the actual space and initializes those data to be the
// ends of the list.  Initializes length and size to 0.
void el_init_blocklist(el_blocklist_t *list){
  list->beg        = &(list->beg_actual); 
  list->beg->state = EL_BEGIN_BLOCK;
  list->beg->size  = EL_UNINITIALIZED;
  list->end        = &(list->end_actual); 
  list->end->state = EL_END_BLOCK;
  list->end->size  = EL_UNINITIALIZED;
  list->beg->next  = list->end;
  list->beg->prev  = NULL;
  list->end->next  = NULL;
  list->end->prev  = list->beg;
  list->length     = 0;
  list->bytes      = 0;
}  

// REQUIRED
// Add to the front of list; links for block are adjusted as are links
// within list.  Length is incremented and the bytes for the list are
// updated to include the new block's size and its overhead.
void el_add_block_front(el_blocklist_t *list, el_blockhead_t *block){
  // Set the next node for the block to the block after the beginning of the list
  // Attach listed nodes to the block
  block->next = list->beg->next;
  block->next->prev = block;

  // Set the previous node for the block to the beginning of the list=
  // Attach beginning of list to the block
  block->prev = list->beg;
  block->prev->next = block;

  // Increment list length
  // Update list size
  (list->length)++;
  list->bytes += block->size + EL_BLOCK_OVERHEAD;
}

// REQUIRED
// Unlink block from the list it is in which should be the list
// parameter.  Updates the length and bytes for that list including
// the EL_BLOCK_OVERHEAD bytes associated with header/footer.
void el_remove_block(el_blocklist_t *list, el_blockhead_t *block){
  // Cut out this node by linking the two neighboring nodes to eachother
  block->next->prev = block->prev;
  block->prev->next = block->next;
  
  (list->length)--;                                    // Deincrement the length counter
  list->bytes -= block->size + EL_BLOCK_OVERHEAD;      // Remove the size of the block and it's overhead to the byte-size counter
}

////////////////////////////////////////////////////////////////////////////////
// Allocation-related functions

// REQUIRED
// Find the first block in the available list with block size of at
// least `size`.  Returns a pointer to the found block or NULL if no
// block of sufficient size is available.
el_blockhead_t *el_find_first_avail(size_t size){
  el_blockhead_t *block = el_ctl->avail->beg;

  // Iterate through each block. If end is not next node:
  while(block->next != el_ctl->avail->end){
    // Get next block
    block = block->next;

    // Check if block is avalible. // Check if block size is large enough. If yes, return block
    if (block->state == EL_AVAILABLE) if(block->size >= size) return block;
  }

  // No returnable block found: return NULL
  return NULL;
}

// REQUIRED
// Set the pointed to block to the given size and add a footer to
// it. Creates another block above it by creating a new header and
// assigning it the remaining space. Ensures that the new block has a
// footer with the correct size. Returns a pointer to the newly
// created block while the parameter block has its size altered to
// parameter size. Does not do any linking of blocks.  If the
// parameter block does not have sufficient size for a split (at least
// new_size + EL_BLOCK_OVERHEAD for the new header/footer) makes no
// changes tot the block and returns NULL indicating no new block was
// created.
el_blockhead_t *el_split_block(el_blockhead_t *block, size_t size_NEW){
  // Check if there's not enough space to split. If there isn't, return NULL
  if (block->size < (size_NEW + EL_BLOCK_OVERHEAD)) return NULL;

  // Get foot of the current block, and save the block's current size
  el_blockfoot_t* foot = el_get_footer(block); 
  size_t size_OLD = block->size; 

  // Change the block's designated size to size_NEW
  block->size = size_NEW; 

  // Create a new foot pointer to the foot of block, and set it to the new size
  el_blockfoot_t* foot_NEW = el_get_footer(block); 
  foot_NEW->size = size_NEW; 

  // Create a new header (block_NEW) in the memory space left by block we just shrank
  // Set new block's size equal to the remainder of the old block size - the new block size
  el_blockhead_t* block_NEW = el_block_above(block); 
  block_NEW->size = size_OLD - (size_NEW + EL_BLOCK_OVERHEAD);  
  block_NEW->state = EL_AVAILABLE;

  // The original foot pointer we got is now the foot of the new block, so set it's size to the size of the new block
  foot->size = block_NEW->size; 

  // Return a pointer to the new block
  return block_NEW;
}

// REQUIRED
// Return pointer to a block of memory with at least the given size
// for use by the user.  The pointer returned is to the usable space,
// not the block header. Makes use of find_first_avail() to find a
// suitable block and el_split_block() to split it.  Returns NULL if
// no space is available.
void *el_malloc(size_t nbytes){
  // Locate a block of nbytes size or larger
  // If no such block exists, return NULL
  el_blockhead_t* block = el_find_first_avail(nbytes);
  if (block == NULL) return NULL;

  // Remove the located block from the control heap
  el_remove_block(el_ctl->avail, block); 

  // For the block of memory thats >= nbytes, split any excess off so that it's exactly nbytes big
  el_blockhead_t* new_block = el_split_block(block, nbytes);

  // If any excess space is returned (as new_block), add it back into the 'avalible' list of the control heap
  if (new_block != NULL) el_add_block_front(el_ctl->avail, new_block);   
  
  // Set the block's state to USED
  // Add it to the front of the 'used' list of the control heap
  block->state = EL_USED;
  el_add_block_front(el_ctl->used, block);
  
  // Returns a pointer to the block
  return PTR_PLUS_BYTES(block,sizeof(el_blockhead_t));
}

////////////////////////////////////////////////////////////////////////////////
// De-allocation/free() related functions

// REQUIRED
// Attempt to merge the block lower with the next block in
// memory. Does nothing if lower is null or not EL_AVAILABLE and does
// nothing if the next higher block is null (because lower is the last
// block) or not EL_AVAILABLE.  Otherwise, locates the next block with
// el_block_above() and merges these two into a single block. Adjusts
// the fields of lower to incorporate the size of higher block and the
// reclaimed overhead. Adjusts footer of higher to indicate the two
// blocks are merged.  Removes both lower and higher from the
// available list and re-adds lower to the front of the available
// list.
void el_merge_block_with_above(el_blockhead_t *lower){
  // Check if the block in the argument is NULL. If so, return.
  if (lower == NULL) return;

  // 'lower' is not NULL. Check if the block in the argument is avalible. If so, continue,
  if (lower->state == EL_AVAILABLE){
    // Get the pointer for the block in memory above the argument 'lower' (named 'higher')
    el_blockhead_t* higher = el_block_above(lower);

    // Check if the 'higher' block is NULL. If so, return.
    if (higher == NULL) return;

    // 'higher' is not NULL. Check if the 'higher' block is avalibleL. If so: merge lower and higher
    if (higher->state == EL_AVAILABLE) {
      // Remove 'lower' and 'higher' from the 'avalible' lists 
      el_remove_block(el_ctl->avail,higher);
      el_remove_block(el_ctl->avail,lower);
    
      // Get the combined size of 'lower' and 'higher' plus overhead 
      size_t new_size = lower->size + higher->size + EL_BLOCK_OVERHEAD;

      // Set the size of 'lower' to this combined size, and update it's footer accordingly
      lower->size = new_size;
      el_get_footer(lower)->size = new_size;

      // Add the newly merged 'lower' back into the 'avalible' list
      el_add_block_front(el_ctl->avail,lower);

      // Recursively call the function again with the merged 'lower' to check if the next block should also be merged
      el_merge_block_with_above(lower);
    }

    // Get the pointer for the block in memory below the argument 'lower' (named 'lowerer')
    el_blockhead_t* lowerer = el_block_below(lower);

    // Check if the 'lowerer' block is NULL. If so, return.
    if (lowerer == NULL) return;
    
    // 'lowerer' is not NULL. Check if the 'lowerer' block is avalible.
    //  If so, recursively call the function with 'lowerer' to try and merge it with 'lower'
    if (lowerer->state == EL_AVAILABLE) 
        el_merge_block_with_above(lowerer);
  }
}

// REQUIRED
// Free the block pointed to by the give ptr.  The area immediately
// preceding the pointer should contain an el_blockhead_t with information
// on the block size. Attempts to merge the free'd block with adjacent
// blocks using el_merge_block_with_above().
void el_free(void *ptr){
  // Get the block pointed to by pointer 'ptr'
  el_blockhead_t *free = PTR_MINUS_BYTES(ptr, sizeof(el_blockhead_t));

  // Remove the pointed to block from the 'used' control heap list, and set the block's state to 'Avalible'
  el_remove_block(el_ctl->used, free);
  free->state = EL_AVAILABLE;

  // Add the block to the 'avalible' control heap list, then attempt to merge it with any agacent blocks in memory
  el_add_block_front(el_ctl->avail, free);
  el_merge_block_with_above(free);
}

////////////////////////////////////////////////////////////////////////////////
// HEAP EXPANSION FUNCTIONS

// REQUIRED
// Attempts to append pages of memory to the heap with mmap(). npages
// is how many pages are to be appended with total bytes to be
// appended as npages * EL_PAGE_BYTES. Calls mmap() with similar
// arguments to those used in el_init() however requests the address
// of the pages to be at heap_end so that the heap grows
// contiguously. If this fails, prints the message
// 
//  ERROR: Unable to mmap() additional 3 pages
// 
// and returns 1. Note that mmap() returns the constant MAP_FAILED on
// errors and the returned address will not match the requested
// virtual address on failures.
//
// Otherwise, adjusts heap size and end for the expanded heap. Creates
// a new block for the freshly allocated pages that is added to the
// available list. Also attempts to merge this block with the block
// below it. Returns 0 on success.
int el_append_pages_to_heap(int npages){
 
    size_t new_size = npages * EL_PAGE_BYTES;

    // Create a new heap that maps pages to yjr
    void *new_heap_segment = mmap(el_ctl->heap_end, new_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (new_heap_segment == MAP_FAILED) {
        fprintf(stderr, "ERROR: Unable to mmap() additional %d pages\n", npages);

        // Return 1: failure to expand heap
        return 1; 
    }

    // Check that if the memory was mapped correctly
    // If not, unmap the segment and print an error message.
    if (new_heap_segment != el_ctl->heap_end) {
        munmap(new_heap_segment, new_size);
        fprintf(stderr, "ERROR: Unable to mmap() additional %d pages\n", npages); 
        
        // Return 1: failure to expand heap
        return 1;
    }

    // Update heap control structure
    el_ctl->heap_end = (char*)el_ctl->heap_end + new_size;
    el_ctl->heap_bytes += new_size;

    // Create a new block at the newly appended area
    el_blockhead_t *new_block = (el_blockhead_t *)new_heap_segment;
    new_block->size = new_size - EL_BLOCK_OVERHEAD;
    new_block->state = EL_AVAILABLE;

    el_blockfoot_t *new_foot = el_get_footer(new_block);
    new_foot->size = new_block->size;

    // Integrate the new block into the available list
    el_add_block_front(el_ctl->avail, new_block);

    // Attempt to merge the new block with the previous block if it's also available
    el_blockhead_t *prev_block = el_block_below(new_block);
    if (prev_block && prev_block->state == EL_AVAILABLE) {
        el_merge_block_with_above(prev_block);  
    }

    return 0; // Success
}
