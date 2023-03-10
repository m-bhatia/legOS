#include "scheduling.h" 
#include "i8259.h"
#include "terminal.h"
#include "syscall.h"
#include "paging.h"

int32_t curr_pid;
uint32_t startUpInitialized = 0;

/*              General Notes about Scheduling              */
/* 1) Need to support up to 3 terminals and use             */
/*    ALT+F(1,2,3) to switch between the terminals can have */
/*    them all running at the start or have user open them  */
/*                                                          */
/* 2) Need to be able to support up to 6 processes at once  */
/*    make sure we don't crash when opening the 7th.        */
/*    an instance of shell counts as a process              */
/*                                                          */
/* 3) Need a seperate input buffer with each terminal need  */
/*    to save the current text screen/cursor                */
/*                                                          */
/* 4) Have a space in virtual memeory that stores the last  */
/*    saved state of each terminal, when switching          */
/*    terminals save the current one in its correct         */
/*    location and then load the new terminal in the 4kB    */
/*    video memeory location (use memcpy?)                  */
/*                                                          */
/* 5) Then switch the current execution to the new terminal */
/*    user program                                          */
/*                                                          */
/* 6) Use PIT instead of RTC to set frequencies better and  */
/*    set a higher priority in the PIC                      */
/*                                                          */
/* 7) Typing should appear on the visible terminal not the  */
/*    scheduled terminal                                    */
/*                                                          */
/* 8) Use the kernel stack, use assebly for the context     */
/*    switch, switch ESP/EBP to the next process' kernel    */
/*    stack, restore the next process' TSS, and Flush TLB   */
/*    on process switch                                     */
/*                                                          */
/* 9) The PIT handler should execute the sched algorithm    */
/*                                                          */


/* --------------------- PIT_INIT --------------------- */
/* Initializes the PIT (Programmable Interval Timer),   */
/* enabling us to implement the scheduler. The PIT chip */
/* consists of an oscillator, a prescaler, and three    */
/* independent frequency dividers. Each divider has     */
/* an ouptut, which is used to allow the timer to       */
/* control external circuitry (for example, IRQ 0).     */
/* More information at https://wiki.osdev.org/PIT       */
/* Inputs:          None.                               */
/* Outputs:         None.                               */
/* Side Effects:    Initializes the PIT to usage in     */
/*                  scheduling.                         */
void PIT_init( void )
{
    /* Set the PIT Command Register to Square Wave Mode */
    /* to enable the PIT to generate Square Waves for   */
    /* timings. Command register number located under   */
    /* "I/O Ports" on osdev.org provided above.         */
    outb( PIT_COMMAND_REG_VAL, PIT_COMMAND_REG );

    /* Also set the frequency of channel 0 by setting   */
    /* the high and low bytes of the channel.           */
    uint16_t reload = RELOAD_VAL;
    outb( reload & RELOAD_MASK_LOWER, CHANNEL_0 );
    outb( reload & RELOAD_MASK_UPPER, CHANNEL_0 );

    /* Now setup the PIT with PIC to enable interrupts  */
    enable_irq(PIT_IRQ_NUM);
}

/* Called whenever an interrupt is generated by the PIT */
/* Will cause the next task in the round robin          */
/* scheduling to occur                                  */
/* Inputs:          None.                               */
/* Outputs:         None.                               */
/* Side effects:    Switches to next task in the round  */
/*                  robin schedule                      */
void pit_handler( void ){
    cli();                          /* Disable interrupts   */
    scheduler();                    /* Call the scheduler   */
    send_eoi(PIT_IRQ_NUM);          /* Send EOI to the PIC  */
    /* RESUME: Check send_eoi stuff */
    sti();                          /* Enable interrupts    */
}

/* Called by pit_handler whenever an interrupt is       */
/* generated by the PIT.                                */
/* Causes the next task in the round robin              */
/* Inputs:          None.                               */
/* Outputs:         None.                               */
/* Side effects:    Causes switch to next task in round */
/*                  robin schedule                      */
void scheduler( void ){
    // cli();

    /* Store the ESP and the EBP so that we can return to it later */
    uint32_t saved_esp;
    uint32_t saved_ebp;
    asm volatile( "movl     %%esp, %[saved_esp];"
                  "movl     %%ebp, %[saved_ebp];"
                  : /* Output operands. C variables that the asm code   */
                    /* will output into.                                */
                    [saved_esp] "=m" (saved_esp), 
                    [saved_ebp] "=m" (saved_ebp)
                  : /* Input Operands. C variables that the asm code    */
                    /* will use as inputs. No input operands used.      */
                  : /* Clobbers and Special Registers. Indicators that  */
                    /* the compiler should use special register or that */
                    /* certain components get clobbered. "Memory" gets  */
                    /* clobbered here.                                  */
                    "memory"
                ); 
    /* Make sure to save ESP, and EBP into the terminals struct array */
    terminals[sched_terminal].pid = curr_pid;
    terminals[sched_terminal].saved_esp = saved_esp;
    terminals[sched_terminal].saved_ebp = saved_ebp;

    /* If the current terminal is not initialized, set up   */
    /* and execute shell                                    */
    if (terminals[sched_terminal].initialized == 0) {
        /* Sets the terminal to be marked as initialized    */
        terminals[sched_terminal].initialized = 1;

        /* Properly switches the memory of the old terminal to be that of   */
        /* the next scheduled terminal                                      */
        switch_terminal(sched_terminal);     

        /* Need to send the end-of-interrupt signal before execute is called */
        send_eoi(PIT_IRQ_NUM);    

        /* Executes the base shell call */
        syscall_execute( (uint8_t*)"shell" );

        return;
    }  
    
    /* Switch to the next terminal in a round robin loop */
    /* Terminal 2 --> 1 --> 0 --> 2 , etc.               */
    sched_terminal -= 1;
    if (sched_terminal < 0) {
        sched_terminal = 2;
    }     

    #if 0
    /* Return early if the next terminal in the round robin is not initialized */
    if (terminals[sched_terminal].initialized == 0) {
        return;
    }

    if (sched_terminal == display_terminal) {
        set_video_page_to_reg( );
    } else {
        set_non_displayed_video_page( sched_terminal );
    }

    /* Gets the current process ID and the saved values for ESP and EBP */
    int32_t next_pid = terminals[sched_terminal].pid;
    saved_esp = terminals[sched_terminal].saved_esp;
    saved_ebp = terminals[sched_terminal].saved_ebp;

    if (next_pid > 2) {
        /* Remaps the corresponding program based off of the program ID to the user page */
        map_prog_to_page(next_pid);
    }
    
  
    /* Updates tss parameters to prepare for context switch */
    tss.ss0 = KERNEL_DS;
    tss.esp0 = EIGHT_MB - EIGHT_KB * curr_pid - 4;

    /* Send end-of-interrupt signal for PIT to the PIC */
    // send_eoi(PIT_IRQ_NUM);

    /* Context switch to the next program in the scheduling queue */
    asm volatile( 
                    "movl     %0, %%esp;" /* Move arg one into reg ESP    */
                    "movl     %1, %%ebp;" /* Move arg two into reg EBP    */
                    : /* No output operands used. */
                    : /* Input Operands.          */
                      /* Input 0: Saved ESP.      */
                      "r" ( saved_esp ),
                      /* Input 1: Saved EBP.      */
                      "r" ( saved_ebp )
                ); 
    

    // sti();
    #endif
    return;
} 

/* PAGING FUNCTIONS RELEVANT TO SCHEDULER */
/* ---------------- set_video_page -------------------- */
/* Sets characteristics and virtual memory address of   */
/* page to point to the video memory                    */
/* Inputs:          None.                               */
/* Outputs:         None.                               */
/* Side Effects:    Sets the virtual adress of the      */
/*                  vid_page_table to vid_mem           */
void set_video_page_to_reg( void ) {
    
    vid_page_table[0].present = 1;
    vid_page_table[0].read_write = 1;
    vid_page_table[0].user_supervisor = 1;
    vid_page_table[0].virtual_address = VIDEO_START_ADDR / FOUR_KB;

    flush_tlb();
}

/* --------- set_alternative_video_page --------------- */
/* Sets characteristis and virtual memory address of    */
/* page to point to the saved video memory coorsponding */
/* to the passed terminal                               */
/* Inputs:          Serminal we are writing to.         */
/* Outputs:         None.                               */
/* Side Effects:    Sets the virtual adress of the      */
/*                  vid_page_table to vid_mem + offset  */
/*                  to terminal we are updating         */
void set_non_displayed_video_page( int terminal )
{

    vid_page_table[0].present = 1;
    vid_page_table[0].read_write = 1;
    vid_page_table[0].user_supervisor = 1;
    vid_page_table[0].virtual_address = ((VIDEO_ALT_START + (terminal)) * SCHED_FOUR_MB) / FOUR_KB;

    flush_tlb();
}
