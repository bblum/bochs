/**
 * @file x86.c
 * @brief x86-specific utilities
 * @author Ben Blum
 */

#define MODULE_NAME "X86"
#define MODULE_COLOUR COLOUR_DARK COLOUR_GREEN

#include "common.h"
#include "kernel_specifics.h"
#include "kspec.h"
#include "student_specifics.h"
#include "x86.h"

#ifdef BOCHS

#include "iodev/iodev.h"

unsigned int cause_timer_interrupt_immediately(cpu_t *cpu)
{
	// TODO: replace with kern get timer wrpa begin
	unsigned int handler = 0x10181e;
	DEV_pic_lower_irq(0);
	DEV_pic_raise_irq(0);
	assert(cpu->async_event);
	assert(!cpu->interrupts_inhibited(BX_INHIBIT_INTERRUPTS));
	assert(cpu->is_unmasked_event_pending(BX_EVENT_PENDING_INTR));
	bool rv = cpu->handleAsyncEvent(); /* modifies eip */
	assert(!rv); /* not need break out of cpu loop */
	assert(!cpu->async_event);
	assert(GET_CPU_ATTR(cpu, eip) == handler);
	return handler;
}

void cause_timer_interrupt(cpu_t *cpu, apic_t *apic, pic_t *pic)
{
	assert(apic == NULL && "not needed");
	assert(pic  == NULL && "not needed");
	DEV_pic_lower_irq(0);
	DEV_pic_raise_irq(0);
	assert(cpu->async_event);
}

unsigned int avoid_timer_interrupt_immediately(cpu_t *cpu)
{
	BX_OUTP(0x20, 0x20, 1);
	// TODO: replace with kerne get timer wrap end
	SET_CPU_ATTR(cpu, eip, 0x101866);
}

static void do_scan(int key_event, bool shift)
{
	if (shift) DEV_kbd_gen_scancode(BX_KEY_SHIFT_L);
	DEV_kbd_gen_scancode(key_event);
	DEV_kbd_gen_scancode(key_event | BX_KEY_RELEASED);
	if (shift) DEV_kbd_gen_scancode(BX_KEY_SHIFT_L | BX_KEY_RELEASED);
}

void cause_keypress(keyboard_t *kbd, char key)
{
	assert(kbd == NULL && "not needed");
	switch (key) {
		case '\n': do_scan(BX_KEY_KP_ENTER, 0); break;
		case '_': do_scan(BX_KEY_MINUS, 1); break;
		case ' ': do_scan(BX_KEY_SPACE, 0); break;
		case 'a': do_scan(BX_KEY_A, 0); break;
		case 'b': do_scan(BX_KEY_B, 0); break;
		case 'c': do_scan(BX_KEY_C, 0); break;
		case 'd': do_scan(BX_KEY_D, 0); break;
		case 'e': do_scan(BX_KEY_E, 0); break;
		case 'f': do_scan(BX_KEY_F, 0); break;
		case 'g': do_scan(BX_KEY_G, 0); break;
		case 'h': do_scan(BX_KEY_H, 0); break;
		case 'i': do_scan(BX_KEY_I, 0); break;
		case 'j': do_scan(BX_KEY_J, 0); break;
		case 'k': do_scan(BX_KEY_K, 0); break;
		case 'l': do_scan(BX_KEY_L, 0); break;
		case 'm': do_scan(BX_KEY_M, 0); break;
		case 'n': do_scan(BX_KEY_N, 0); break;
		case 'o': do_scan(BX_KEY_O, 0); break;
		case 'p': do_scan(BX_KEY_P, 0); break;
		case 'q': do_scan(BX_KEY_Q, 0); break;
		case 'r': do_scan(BX_KEY_R, 0); break;
		case 's': do_scan(BX_KEY_S, 0); break;
		case 't': do_scan(BX_KEY_T, 0); break;
		case 'u': do_scan(BX_KEY_U, 0); break;
		case 'v': do_scan(BX_KEY_V, 0); break;
		case 'w': do_scan(BX_KEY_W, 0); break;
		case 'x': do_scan(BX_KEY_X, 0); break;
		case 'y': do_scan(BX_KEY_Y, 0); break;
		case 'z': do_scan(BX_KEY_Z, 0); break;
		case '0': do_scan(BX_KEY_0, 0); break;
		case '1': do_scan(BX_KEY_1, 0); break;
		case '2': do_scan(BX_KEY_2, 0); break;
		case '3': do_scan(BX_KEY_3, 0); break;
		case '4': do_scan(BX_KEY_4, 0); break;
		case '5': do_scan(BX_KEY_5, 0); break;
		case '6': do_scan(BX_KEY_6, 0); break;
		case '7': do_scan(BX_KEY_7, 0); break;
		case '8': do_scan(BX_KEY_8, 0); break;
		case '9': do_scan(BX_KEY_9, 0); break;
		default: assert(0 && "keypress not implemented");
	}
}

bool interrupts_enabled(cpu_t *cpu)
{
	assert(0 && "unimplemented");
	return false;
}

unsigned int delay_instruction(cpu_t *cpu)
{
	assert(0 && "unimplemented");
}

#else
#include "x86-simics.c"
#endif

static bool mem_translate(cpu_t *cpu, unsigned int addr, unsigned int *result)
{
#ifdef PINTOS_KERNEL
	/* In pintos the kernel is mapped at 3 GB, not direct-mapped. Luckily,
	 * paging is enabled in start(), while landslide enters at main(). */
	assert((GET_CR0(cpu) & CR0_PG) != 0 &&
	       "Expected Pintos to have paging enabled before landslide entrypoint.");
#else
	/* In pebbles the kernel is direct-mapped and paging may not be enabled
	 * until after landslide starts recording instructions. */
	if (KERNEL_MEMORY(addr)) {
		/* assume kern mem direct-mapped -- not strictly necessary */
		*result = addr;
		return true;
	} else if ((GET_CR0(cpu) & CR0_PG) == 0) {
		/* paging disabled; cannot translate user address */
		return false;
	}
#endif

	unsigned int upper = addr >> 22;
	unsigned int lower = (addr >> 12) & 1023;
	unsigned int offset = addr & 4095;
	unsigned int cr3 = GET_CR3(cpu);
	unsigned int pde_addr = cr3 + (4 * upper);
	unsigned int pde = READ_PHYS_MEMORY(cpu, pde_addr, WORD_SIZE);
	/* check present bit of pde to not anger the simics gods */
	if ((pde & 0x1) == 0) {
		return false;
#ifdef PDE_PTE_POISON
	} else if (pde == PDE_PTE_POISON) {
		return false;
#endif
	}
	unsigned int pte_addr = (pde & ~4095) + (4 * lower);
	unsigned int pte = READ_PHYS_MEMORY(cpu, pte_addr, WORD_SIZE);
	/* check present bit of pte to not anger the simics gods */
	if ((pte & 0x1) == 0) {
		return false;
#ifdef PDE_PTE_POISON
	} else if (pte == PDE_PTE_POISON) {
		return false;
#endif
	}
	*result = (pte & ~4095) + offset;
	return true;
}

unsigned int read_memory(cpu_t *cpu, unsigned int addr, unsigned int width)
{
	unsigned int phys_addr;
	if (mem_translate(cpu, addr, &phys_addr)) {
		return READ_PHYS_MEMORY(cpu, phys_addr, width);
	} else {
		return 0; /* :( */
	}
}

bool write_memory(cpu_t *cpu, unsigned int addr, unsigned int val, unsigned int width)
{
	unsigned int phys_addr;
	if (mem_translate(cpu, addr, &phys_addr)) {
		WRITE_PHYS_MEMORY(cpu, phys_addr, val, width);
		return true;
	} else {
		return false;
	}
}

char *read_string(cpu_t *cpu, unsigned int addr)
{
	unsigned int length = 0;

	while (READ_BYTE(cpu, addr + length) != 0) {
		length++;
	}

	char *buf = MM_XMALLOC(length + 1, char);

	for (unsigned int i = 0; i <= length; i++) {
		buf[i] = READ_BYTE(cpu, addr + i);
	}

	return buf;
}

/* will read at most 3 opcodes */
bool opcodes_are_atomic_swap(uint8_t *ops) {
	unsigned int offset = 0;
	if (ops[offset] == 0xf0) {
		/* lock prefix */
		offset++;
	}

	if (ops[offset] == 0x86 || ops[offset] == 0x87) {
		/* xchg */
		return true;
	} else if (ops[offset] == 0x0f) {
		offset++;
		if (ops[offset] == 0xb0 || ops[offset] == 0xb1) {
			/* cmpxchg */
			return true;
		} else {
			// FIXME: Shouldn't 0F C0 and 0F C1 (xadd) be here?
			return false;
		}
	} else {
		return false;
	}
}

bool instruction_is_atomic_swap(cpu_t *cpu, unsigned int eip) {
	uint8_t opcodes[3];
	opcodes[0] = READ_BYTE(cpu, eip);
	opcodes[1] = READ_BYTE(cpu, eip + 1);
	opcodes[2] = READ_BYTE(cpu, eip + 2);
	return opcodes_are_atomic_swap(opcodes);
}

unsigned int cause_transaction_failure(cpu_t *cpu, unsigned int status)
{
#ifdef HTM
	/* it'd work in principle but explore/sched shouldn't use it this way */
	assert(status != _XBEGIN_STARTED && "i don't swing like that");
	SET_CPU_ATTR(cpu, eax, status);
	/* because of the 1-instruction delay on timer interrupce after a PP,
	 * we'll be injecting the failure after ebp is pushed in _xbegin. */
	assert(GET_CPU_ATTR(cpu, eip) == HTM_XBEGIN + 1);
	SET_CPU_ATTR(cpu, eip, HTM_XBEGIN_END - 1);
	return HTM_XBEGIN_END - 1;
#else
	assert(0 && "how did this get here? i am not good with HTM");
	return 0;
#endif
}