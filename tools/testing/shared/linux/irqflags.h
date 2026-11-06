/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _TEST_IRQFLAGS_H
#define _TEST_IRQFLAGS_H

/* No kernel hard-IRQ context exists in this native harness. */
#define local_irq_save(flags) do { (flags) = 0; } while (0)
#define local_irq_restore(flags) do { (void)(flags); } while (0)

#endif
