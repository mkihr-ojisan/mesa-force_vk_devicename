/*
 * Copyright (C) 2018 Alyssa Rosenzweig
 * Copyright (C) 2019-2020 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "util/u_memory.h"
#include "compiler.h"

/**
 * A simple SSA-based dead code elimination pass.
 * This pass assumes that no loop header phis are dead.
 */

void
bi_opt_dce(bi_context *ctx, bool partial)
{
   BITSET_WORD *seen =
      calloc(BITSET_WORDS(ctx->ssa_alloc), sizeof(BITSET_WORD));

   bi_foreach_block(ctx, block) {
      if (block->loop_header) {
         bi_foreach_instr_in_block(block, I) {
            if (I->op != BI_OPCODE_PHI) {
               break;
            }

            bi_foreach_ssa_src(I, s) {
               BITSET_SET(seen, I->src[s].value);
            }
         }
      }
   }

   bi_foreach_block_rev(ctx, block) {
      bi_foreach_instr_in_block_safe_rev(block, I) {
         if (block->loop_header && I->op == BI_OPCODE_PHI)
            break;

         bool needed = false;

         bi_foreach_ssa_dest(I, d) {
            if (BITSET_TEST(seen, I->dest[d].value)) {
               needed = true;
            } else if (partial) {
               I->dest[d] = bi_null();
            }
         }

         if (!needed && !bi_side_effects(I)) {
            bi_remove_instruction(I);
         } else {
            bi_foreach_ssa_src(I, s) {
               BITSET_SET(seen, I->src[s].value);
            }
         }
      }
   }

   free(seen);
}

/* Post-RA liveness-based dead code analysis to clean up results of bundling */

uint64_t MUST_CHECK
bi_postra_liveness_ins(uint64_t live, bi_instr *ins)
{
   bi_foreach_dest(ins, d) {
      if (ins->dest[d].type == BI_INDEX_REGISTER) {
         unsigned nr = bi_count_write_registers(ins, d);
         unsigned reg = ins->dest[d].value;
         live &= ~(BITFIELD64_MASK(nr) << reg);
      }
   }

   bi_foreach_src(ins, s) {
      if (ins->src[s].type == BI_INDEX_REGISTER) {
         unsigned nr = bi_count_read_registers(ins, s);
         unsigned reg = ins->src[s].value;
         live |= (BITFIELD64_MASK(nr) << reg);
      }
   }

   return live;
}

static bool
bi_postra_liveness_block(bi_block *blk)
{
   bi_foreach_successor(blk, succ)
      blk->reg_live_out |= succ->reg_live_in;

   uint64_t live = blk->reg_live_out;

   bi_foreach_instr_in_block_rev(blk, ins)
      live = bi_postra_liveness_ins(live, ins);

   bool progress = blk->reg_live_in != live;
   blk->reg_live_in = live;
   return progress;
}

/* Globally, liveness analysis uses a fixed-point algorithm based on a
 * worklist. We initialize a work list with the exit block. We iterate the work
 * list to compute live_in from live_out for each block on the work list,
 * adding the predecessors of the block to the work list if we made progress.
 */

void
bi_postra_liveness(bi_context *ctx)
{
   u_worklist worklist;
   bi_worklist_init(ctx, &worklist);

   bi_foreach_block(ctx, block) {
      block->reg_live_out = block->reg_live_in = 0;

      bi_worklist_push_tail(&worklist, block);
   }

   while (!u_worklist_is_empty(&worklist)) {
      /* Pop off in reverse order since liveness is backwards */
      bi_block *blk = bi_worklist_pop_tail(&worklist);

      /* Update liveness information. If we made progress, we need to
       * reprocess the predecessors
       */
      if (bi_postra_liveness_block(blk)) {
         bi_foreach_predecessor(blk, pred)
            bi_worklist_push_head(&worklist, *pred);
      }
   }

   u_worklist_fini(&worklist);
}

void
bi_opt_dce_post_ra(bi_context *ctx)
{
   bi_postra_liveness(ctx);

   bi_foreach_block_rev(ctx, block) {
      uint64_t live = block->reg_live_out;

      bi_foreach_instr_in_block_rev(block, ins) {
         if (ins->op == BI_OPCODE_DTSEL_IMM)
            ins->dest[0] = bi_null();

         bi_foreach_dest(ins, d) {
            if (ins->dest[d].type != BI_INDEX_REGISTER)
               continue;

            unsigned nr = bi_count_write_registers(ins, d);
            unsigned reg = ins->dest[d].value;
            uint64_t mask = (BITFIELD64_MASK(nr) << reg);
            bool cullable = (ins->op != BI_OPCODE_BLEND);
            cullable &= !bi_get_opcode_props(ins)->sr_write;

            if (!(live & mask) && cullable)
               ins->dest[d] = bi_null();
         }

         live = bi_postra_liveness_ins(live, ins);
      }
   }
}
