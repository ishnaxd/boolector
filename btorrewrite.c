/*  Boolector: Satisfiablity Modulo Theories (SMT) solver.
 *
 *  Copyright (C) 2007 Robert Daniel Brummayer.
 *  Copyright (C) 2007-2012 Armin Biere.
 *
 *  All rights reserved.
 *
 *  This file is part of Boolector.
 *  See COPYING for more information on using this software.
 */

#include "btorrewrite.h"
#include "btorconst.h"
#include "btormem.h"
#include "btorutil.h"

#include <assert.h>

/*------------------------------------------------------------------------*/

/* recursive rewriting bound */
#define BTOR_REC_RW_BOUND 4096

/* iterative rewriting bounds */
#define BTOR_WRITE_CHAIN_NODE_RW_BOUND 20
#define BTOR_READ_OVER_WRITE_DOWN_PROPAGATION_LIMIT 1024

/* other rewriting bounds */
#define BTOR_FIND_AND_NODE_CONTRADICTION_LIMIT 8

#define BTOR_INC_REC_RW_CALL(btor)                             \
  do                                                           \
  {                                                            \
    (btor)->rec_rw_calls++;                                    \
    if ((btor)->rec_rw_calls > (btor)->stats.max_rec_rw_calls) \
      (btor)->stats.max_rec_rw_calls = (btor)->rec_rw_calls;   \
  } while (0)

#define BTOR_DEC_REC_RW_CALL(btor)     \
  do                                   \
  {                                    \
    assert ((btor)->rec_rw_calls > 0); \
    (btor)->rec_rw_calls--;            \
  } while (0)

/*------------------------------------------------------------------------*/

static int
is_const_one_exp (Btor *btor, BtorNode *exp)
{
  int result;
  BtorNode *real_exp;

  assert (btor);
  assert (exp);
  exp = btor_pointer_chase_simplified_exp (btor, exp);

  if (!BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (exp))) return 0;

  real_exp = BTOR_REAL_ADDR_NODE (exp);
  if (BTOR_IS_INVERTED_NODE (exp)) btor_invert_const (btor->mm, real_exp->bits);
  result = btor_is_special_const (real_exp->bits) == BTOR_SPECIAL_CONST_ONE;
  if (BTOR_IS_INVERTED_NODE (exp)) btor_invert_const (btor->mm, real_exp->bits);

  return result;
}

static int
is_const_zero_exp (Btor *btor, BtorNode *exp)
{
  int result;
  BtorNode *real_exp;

  assert (btor);
  assert (exp);
  exp = btor_pointer_chase_simplified_exp (btor, exp);

  if (!BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (exp))) return 0;

  real_exp = BTOR_REAL_ADDR_NODE (exp);
  if (BTOR_IS_INVERTED_NODE (exp)) btor_invert_const (btor->mm, real_exp->bits);
  result = btor_is_special_const (real_exp->bits) == BTOR_SPECIAL_CONST_ZERO;
  if (BTOR_IS_INVERTED_NODE (exp)) btor_invert_const (btor->mm, real_exp->bits);

  return result;
}

static int
is_xor_exp (Btor *btor, BtorNode *exp)
{
  BtorNode *e0, *e1, *e0_0, *e0_1, *e1_0, *e1_1;

  assert (btor);
  assert (exp);
  exp = btor_pointer_chase_simplified_exp (btor, exp);
  (void) btor;

  if (BTOR_REAL_ADDR_NODE (exp)->kind != BTOR_AND_NODE) return 0;

  e0 = BTOR_REAL_ADDR_NODE (exp)->e[0];
  if (!(BTOR_IS_INVERTED_NODE (e0)
        && BTOR_REAL_ADDR_NODE (e0)->kind == BTOR_AND_NODE))
    return 0;

  e1 = BTOR_REAL_ADDR_NODE (exp)->e[1];
  if (!(BTOR_IS_INVERTED_NODE (e1)
        && BTOR_REAL_ADDR_NODE (e1)->kind == BTOR_AND_NODE))
    return 0;

  e0_0 = BTOR_REAL_ADDR_NODE (e0)->e[0];
  e0_1 = BTOR_REAL_ADDR_NODE (e0)->e[1];
  e1_0 = BTOR_REAL_ADDR_NODE (e1)->e[0];
  e1_1 = BTOR_REAL_ADDR_NODE (e1)->e[1];

  /* we assume that the children of commutative operators are sorted by id */
  /* are children of e0 the same children as of e1 (ignoring sign) ? */
  /* if not we terminate with false */
  if (BTOR_REAL_ADDR_NODE (e0_0) != BTOR_REAL_ADDR_NODE (e1_0)) return 0;
  if (BTOR_REAL_ADDR_NODE (e0_1) != BTOR_REAL_ADDR_NODE (e1_1)) return 0;

  /* we check for two cases */
  /* first case: !(!a && !b) && !(a && b) */
  if (!BTOR_IS_INVERTED_NODE (exp))
  {
    if (BTOR_IS_INVERTED_NODE (e0_0) == BTOR_IS_INVERTED_NODE (e0_1)
        && BTOR_IS_INVERTED_NODE (e1_0) == BTOR_IS_INVERTED_NODE (e1_1)
        && BTOR_IS_INVERTED_NODE (e0_0) != BTOR_IS_INVERTED_NODE (e1_0))
      return 1;
  }
  /* second case: !((!a && b) && !(a && !b)) */
  else
  {
    if (BTOR_IS_INVERTED_NODE (e0_0) != BTOR_IS_INVERTED_NODE (e1_0)
        && BTOR_IS_INVERTED_NODE (e0_1) != BTOR_IS_INVERTED_NODE (e1_1)
        && BTOR_IS_INVERTED_NODE (e0_0) != BTOR_IS_INVERTED_NODE (e0_1))
      return 1;
  }
  return 0;
}

static int
is_xnor_exp (Btor *btor, BtorNode *exp)
{
  assert (btor);
  assert (exp);
  exp = btor_pointer_chase_simplified_exp (btor, exp);
  return is_xor_exp (btor, BTOR_INVERT_NODE (exp));
}

BtorNode *
btor_rewrite_slice_exp (Btor *btor, BtorNode *exp, int upper, int lower)
{
  char *bits = 0;
  int len;
  BtorNode *result, *tmp, *real_exp;
  BtorMemMgr *mm;

  exp = btor_pointer_chase_simplified_exp (btor, exp);
  assert (btor_precond_slice_exp_dbg (btor, exp, upper, lower));
  assert (btor->rewrite_level > 0);

  mm       = btor->mm;
  result   = 0;
  real_exp = BTOR_REAL_ADDR_NODE (exp);
  len      = real_exp->len;

  if (len == upper - lower + 1) /* handles result->len == 1 */
    result = btor_copy_exp (btor, exp);
  else if (BTOR_IS_BV_CONST_NODE (real_exp))
  {
    bits   = btor_slice_const (mm, real_exp->bits, upper, lower);
    result = btor_const_exp (btor, bits);
    result = BTOR_COND_INVERT_NODE (exp, result);
    btor_delete_const (mm, bits);
  }
  else if (real_exp->kind == BTOR_SLICE_NODE)
  {
    if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
      goto BTOR_REWRITE_SLICE_NODE_NO_REWRITE;
    BTOR_INC_REC_RW_CALL (btor);
    result =
        btor_rewrite_slice_exp (btor,
                                BTOR_COND_INVERT_NODE (exp, real_exp->e[0]),
                                real_exp->lower + upper,
                                real_exp->lower + lower);
    BTOR_DEC_REC_RW_CALL (btor);
  }
  /* check if slice and child of concat matches */
  else if (real_exp->kind == BTOR_CONCAT_NODE)
  {
    if (lower == 0
        && BTOR_REAL_ADDR_NODE (real_exp->e[1])->len == upper - lower + 1)
    {
      if (BTOR_IS_INVERTED_NODE (exp))
        result = BTOR_INVERT_NODE (btor_copy_exp (btor, real_exp->e[1]));
      else
        result = btor_copy_exp (btor, real_exp->e[1]);
    }
    else if (btor->rewrite_level < 3)
    {
      /* we look just one level down */
      if (upper == len - 1
          && BTOR_REAL_ADDR_NODE (real_exp->e[0])->len == upper - lower + 1)
      {
        if (BTOR_IS_INVERTED_NODE (exp))
          result = BTOR_INVERT_NODE (btor_copy_exp (btor, real_exp->e[0]));
        else
          result = btor_copy_exp (btor, real_exp->e[0]);
      }
    }
    else
    {
      assert (btor->rewrite_level >= 3);
      /* concats are normalized at rewrite level 3 */
      /* we recursively check if slice and child of concat matches */
      len = BTOR_REAL_ADDR_NODE (real_exp->e[1])->len;
      if (lower >= len)
      {
        if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
          goto BTOR_REWRITE_SLICE_NODE_NO_REWRITE;
        BTOR_INC_REC_RW_CALL (btor);
        result =
            btor_rewrite_slice_exp (btor,
                                    BTOR_COND_INVERT_NODE (exp, real_exp->e[0]),
                                    upper - len,
                                    lower - len);
        BTOR_DEC_REC_RW_CALL (btor);
      }
      else if (upper < len)
      {
        if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
          goto BTOR_REWRITE_SLICE_NODE_NO_REWRITE;
        BTOR_INC_REC_RW_CALL (btor);
        result = btor_rewrite_slice_exp (
            btor, BTOR_COND_INVERT_NODE (exp, real_exp->e[1]), upper, lower);
        BTOR_DEC_REC_RW_CALL (btor);
      }
      else if (lower == 0)
      {
        assert (upper >= len);
        if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
          goto BTOR_REWRITE_SLICE_NODE_NO_REWRITE;
        BTOR_INC_REC_RW_CALL (btor);
        tmp = btor_rewrite_slice_exp (
            btor, BTOR_COND_INVERT_NODE (exp, real_exp->e[0]), upper - len, 0);
        result = btor_rewrite_concat_exp (
            btor, tmp, BTOR_COND_INVERT_NODE (exp, real_exp->e[1]));
        BTOR_DEC_REC_RW_CALL (btor);
        btor_release_exp (btor, tmp);
      }
    }
  }

  if (!result)
  {
  BTOR_REWRITE_SLICE_NODE_NO_REWRITE:
    result = btor_slice_exp_node (btor, exp, upper, lower);
  }

  return result;
}

static BtorNode *
rewrite_binary_exp (Btor *btor, BtorNodeKind kind, BtorNode *e0, BtorNode *e1)
{
  BtorMemMgr *mm;
  BtorNode *result, *real_e0, *real_e1, *tmp1, *tmp2, *tmp3;
  BtorNode *tmp4, *eq, *left, *right;
  BtorNode *(*fptr) (Btor *, BtorNode *, BtorNode *);
  char *b0, *b1, *bresult;
  int same_children_mem;
  int invert_b0 = 0;
  int invert_b1 = 0;
  BtorSpecialConst sc;
  BtorNodePtrStack stack;
  char *bv_const;
  char tmpString[2] = {'\0', '\0'};
  int pos, len;

  e0 = btor_pointer_chase_simplified_exp (btor, e0);
  e1 = btor_pointer_chase_simplified_exp (btor, e1);
  assert (btor);
  assert (btor->rewrite_level > 0);
  assert (BTOR_IS_BINARY_NODE_KIND (kind));
  assert (e0);
  assert (e1);

  mm      = btor->mm;
  result  = 0;
  real_e0 = BTOR_REAL_ADDR_NODE (e0);
  real_e1 = BTOR_REAL_ADDR_NODE (e1);
  if (!real_e0 || !real_e1) abort ();  // make static anlayzer happy
  if (BTOR_IS_BV_CONST_NODE (real_e0) && BTOR_IS_BV_CONST_NODE (real_e1))
  {
    same_children_mem = real_e0 == real_e1;
    if (same_children_mem)
    {
      b0 = BTOR_BITS_NODE (mm, e0);
      b1 = BTOR_BITS_NODE (mm, e1);
    }
    else
    {
      invert_b0 = BTOR_IS_INVERTED_NODE (e0);
      b0        = real_e0->bits;
      if (invert_b0) btor_invert_const (mm, b0);
      invert_b1 = BTOR_IS_INVERTED_NODE (e1);
      b1        = real_e1->bits;
      if (invert_b1) btor_invert_const (mm, b1);
    }
    switch (kind)
    {
      case BTOR_AND_NODE: bresult = btor_and_const (mm, b0, b1); break;
      case BTOR_BEQ_NODE: bresult = btor_eq_const (mm, b0, b1); break;
      case BTOR_ADD_NODE: bresult = btor_add_const (mm, b0, b1); break;
      case BTOR_MUL_NODE: bresult = btor_mul_const (mm, b0, b1); break;
      case BTOR_ULT_NODE: bresult = btor_ult_const (mm, b0, b1); break;
      case BTOR_UDIV_NODE: bresult = btor_udiv_const (mm, b0, b1); break;
      case BTOR_UREM_NODE: bresult = btor_urem_const (mm, b0, b1); break;
      case BTOR_SLL_NODE: bresult = btor_sll_const (mm, b0, b1); break;
      case BTOR_SRL_NODE: bresult = btor_srl_const (mm, b0, b1); break;
      default:
        assert (kind == BTOR_CONCAT_NODE);
        bresult = btor_concat_const (mm, b0, b1);
        break;
    }
    if (same_children_mem)
    {
      btor_delete_const (mm, b1);
      btor_delete_const (mm, b0);
    }
    else
    {
      /* invert back if necessary */
      if (invert_b0) btor_invert_const (mm, b0);
      if (invert_b1) btor_invert_const (mm, b1);
    }
    result = btor_const_exp (btor, bresult);
    btor_delete_const (mm, bresult);
  }
  else if (BTOR_IS_BV_CONST_NODE (real_e0) && !BTOR_IS_BV_CONST_NODE (real_e1))
  {
    invert_b0 = BTOR_IS_INVERTED_NODE (e0);
    b0        = real_e0->bits;
    if (invert_b0) btor_invert_const (mm, b0);
    sc = btor_is_special_const (b0);
    /* invert back if necessary */
    if (invert_b0) btor_invert_const (mm, b0);
    switch (sc)
    {
      case BTOR_SPECIAL_CONST_ZERO:
        switch (kind)
        {
          case BTOR_BEQ_NODE:
            if (real_e0->len == 1)
              result = btor_not_exp (btor, e1);
            else if (is_xor_exp (btor, e1)) /* 0 == (a ^ b)  -->  a = b */
            {
              if (btor->rec_rw_calls < BTOR_REC_RW_BOUND)
              {
                BTOR_INC_REC_RW_CALL (btor);
                result = btor_rewrite_eq_exp (
                    btor,
                    BTOR_REAL_ADDR_NODE (
                        BTOR_REAL_ADDR_NODE (BTOR_REAL_ADDR_NODE (e1)->e[0])
                            ->e[0]),
                    BTOR_REAL_ADDR_NODE (
                        BTOR_REAL_ADDR_NODE (BTOR_REAL_ADDR_NODE (e1)->e[0])
                            ->e[1]));
                BTOR_DEC_REC_RW_CALL (btor);
              }
            }
            else if (BTOR_IS_INVERTED_NODE (e1)
                     && real_e1->kind == BTOR_AND_NODE)
            { /* 0 == a | b  -->  a == 0 && b == 0 */
              if (btor->rec_rw_calls < BTOR_REC_RW_BOUND)
              {
                BTOR_INC_REC_RW_CALL (btor);
                left = btor_rewrite_eq_exp (
                    btor, BTOR_INVERT_NODE (real_e1->e[0]), e0);
                right = btor_rewrite_eq_exp (
                    btor, BTOR_INVERT_NODE (real_e1->e[1]), e0);
                result = btor_rewrite_and_exp (btor, left, right);
                BTOR_DEC_REC_RW_CALL (btor);
                btor_release_exp (btor, left);
                btor_release_exp (btor, right);
              }
            }
            break;
          case BTOR_ULT_NODE: /* 0 < a --> a != 0 */
            result = BTOR_INVERT_NODE (btor_rewrite_eq_exp (btor, e0, e1));
            break;
          case BTOR_ADD_NODE: result = btor_copy_exp (btor, e1); break;
          case BTOR_MUL_NODE:
          case BTOR_SLL_NODE:
          case BTOR_SRL_NODE:
          case BTOR_UREM_NODE:
          case BTOR_AND_NODE:
            result = btor_zero_exp (btor, real_e0->len);
            break;
          case BTOR_UDIV_NODE:
            tmp2   = btor_zero_exp (btor, real_e0->len);
            tmp4   = btor_ones_exp (btor, real_e0->len);
            eq     = btor_rewrite_eq_exp (btor, e1, tmp2);
            result = btor_rewrite_cond_exp (btor, eq, tmp4, tmp2);
            btor_release_exp (btor, tmp2);
            btor_release_exp (btor, eq);
            btor_release_exp (btor, tmp4);
            break;
          default: break;
        }
        break;
      case BTOR_SPECIAL_CONST_ONE_ONES:
        assert (real_e0->len == 1);
        if (kind == BTOR_AND_NODE || kind == BTOR_BEQ_NODE
            || kind == BTOR_MUL_NODE)
          result = btor_copy_exp (btor, e1);
        else if (kind == BTOR_ULT_NODE)
          result = btor_false_exp (btor);
        break;
      case BTOR_SPECIAL_CONST_ONE:
        if (kind == BTOR_MUL_NODE) result = btor_copy_exp (btor, e1);
        break;
      case BTOR_SPECIAL_CONST_ONES:
        if (kind == BTOR_BEQ_NODE)
        {
          if (is_xnor_exp (btor, e1)) /* 1+ == (a XNOR b)  -->  a = b */
          {
            if (btor->rec_rw_calls < BTOR_REC_RW_BOUND)
            {
              BTOR_INC_REC_RW_CALL (btor);
              result = btor_rewrite_eq_exp (
                  btor,
                  BTOR_REAL_ADDR_NODE (
                      BTOR_REAL_ADDR_NODE (BTOR_REAL_ADDR_NODE (e1)->e[0])
                          ->e[0]),
                  BTOR_REAL_ADDR_NODE (
                      BTOR_REAL_ADDR_NODE (BTOR_REAL_ADDR_NODE (e1)->e[0])
                          ->e[1]));
              BTOR_DEC_REC_RW_CALL (btor);
            }
          }
          else if (!BTOR_IS_INVERTED_NODE (e1) && e1->kind == BTOR_AND_NODE)
          { /* 1+ == a & b  -->  a == 1+ && b == 1+ */
            if (btor->rec_rw_calls < BTOR_REC_RW_BOUND)
            {
              BTOR_INC_REC_RW_CALL (btor);
              left   = btor_rewrite_eq_exp (btor, e1->e[0], e0);
              right  = btor_rewrite_eq_exp (btor, e1->e[1], e0);
              result = btor_rewrite_and_exp (btor, left, right);
              BTOR_DEC_REC_RW_CALL (btor);
              btor_release_exp (btor, left);
              btor_release_exp (btor, right);
            }
          }
        }
        else if (kind == BTOR_AND_NODE)
          result = btor_copy_exp (btor, e1);
        else if (kind == BTOR_ULT_NODE) /* UNSIGNED_MAX < x */
          result = btor_false_exp (btor);
        break;
      default:
        assert (sc == BTOR_SPECIAL_CONST_NONE);
        if (kind == BTOR_BEQ_NODE && real_e1->kind == BTOR_AND_NODE
            && btor->rec_rw_calls < BTOR_REC_RW_BOUND)
        {
          BTOR_INC_REC_RW_CALL (btor);
          BTOR_INIT_STACK (stack);
          if (BTOR_IS_INVERTED_NODE (e0))
            bv_const = btor_not_const (btor->mm, real_e0->bits);
          else
            bv_const = btor_copy_const (btor->mm, real_e0->bits);

          pos = 0;
          /* const == a | b */
          if (BTOR_IS_INVERTED_NODE (e1))
          {
            while (pos < real_e0->len)
            {
              tmpString[0] = bv_const[pos];
              len          = (int) strspn (bv_const + pos, tmpString);
              tmp1         = btor_rewrite_slice_exp (btor,
                                             BTOR_INVERT_NODE (real_e1->e[0]),
                                             real_e0->len - 1 - pos,
                                             real_e0->len - pos - len);
              tmp2         = btor_rewrite_slice_exp (btor,
                                             BTOR_INVERT_NODE (real_e1->e[1]),
                                             real_e0->len - 1 - pos,
                                             real_e0->len - pos - len);
              if (tmpString[0] == '0')
              {
                tmp3 = btor_zero_exp (btor, len);
                BTOR_PUSH_STACK (
                    btor->mm, stack, btor_rewrite_eq_exp (btor, tmp1, tmp3));
                BTOR_PUSH_STACK (
                    btor->mm, stack, btor_rewrite_eq_exp (btor, tmp2, tmp3));
                btor_release_exp (btor, tmp3);
              }
              else
              {
                assert (tmpString[0] == '1');
                tmp3 = btor_or_exp (btor, tmp1, tmp2);
                tmp4 = btor_ones_exp (btor, len);
                BTOR_PUSH_STACK (
                    btor->mm, stack, btor_rewrite_eq_exp (btor, tmp3, tmp4));
                btor_release_exp (btor, tmp3);
                btor_release_exp (btor, tmp4);
              }
              btor_release_exp (btor, tmp1);
              btor_release_exp (btor, tmp2);
              pos += len;
            }
          }
          else
          {
            assert (!BTOR_IS_INVERTED_NODE (e1));
            /* const == a & b */
            while (pos < real_e0->len)
            {
              tmpString[0] = bv_const[pos];
              len          = (int) strspn (bv_const + pos, tmpString);
              tmp1         = btor_rewrite_slice_exp (btor,
                                             e1->e[0],
                                             real_e0->len - 1 - pos,
                                             real_e0->len - pos - len);
              tmp2         = btor_rewrite_slice_exp (btor,
                                             e1->e[1],
                                             real_e0->len - 1 - pos,
                                             real_e0->len - pos - len);
              if (tmpString[0] == '1')
              {
                tmp3 = btor_ones_exp (btor, len);
                BTOR_PUSH_STACK (
                    btor->mm, stack, btor_rewrite_eq_exp (btor, tmp1, tmp3));
                BTOR_PUSH_STACK (
                    btor->mm, stack, btor_rewrite_eq_exp (btor, tmp2, tmp3));
                btor_release_exp (btor, tmp3);
              }
              else
              {
                assert (tmpString[0] == '0');
                tmp3 = btor_rewrite_and_exp (btor, tmp1, tmp2);
                tmp4 = btor_zero_exp (btor, len);
                BTOR_PUSH_STACK (
                    btor->mm, stack, btor_rewrite_eq_exp (btor, tmp3, tmp4));
                btor_release_exp (btor, tmp3);
                btor_release_exp (btor, tmp4);
              }
              btor_release_exp (btor, tmp1);
              btor_release_exp (btor, tmp2);
              pos += len;
            }
          }

          result = btor_true_exp (btor);
          assert (!BTOR_EMPTY_STACK (stack));
          do
          {
            tmp1 = BTOR_POP_STACK (stack);
            tmp2 = btor_rewrite_and_exp (btor, result, tmp1);
            btor_release_exp (btor, result);
            result = tmp2;
            btor_release_exp (btor, tmp1);
          } while (!BTOR_EMPTY_STACK (stack));
          btor_delete_const (btor->mm, bv_const);
          BTOR_RELEASE_STACK (btor->mm, stack);
          BTOR_DEC_REC_RW_CALL (btor);
        }
        break;
    }
  }
  else if (!BTOR_IS_BV_CONST_NODE (real_e0) && BTOR_IS_BV_CONST_NODE (real_e1))
  {
    invert_b1 = BTOR_IS_INVERTED_NODE (e1);
    b1        = real_e1->bits;
    if (invert_b1) btor_invert_const (mm, b1);
    sc = btor_is_special_const (b1);
    /* invert back if necessary */
    if (invert_b1) btor_invert_const (mm, b1);
    switch (sc)
    {
      case BTOR_SPECIAL_CONST_ZERO:
        switch (kind)
        {
          case BTOR_BEQ_NODE:
            if (real_e0->len == 1)
              result = btor_not_exp (btor, e0);
            else if (is_xor_exp (btor, e0)) /* (a ^ b) == 0 -->  a = b */
            {
              if (btor->rec_rw_calls < BTOR_REC_RW_BOUND)
              {
                BTOR_INC_REC_RW_CALL (btor);
                result = btor_rewrite_eq_exp (
                    btor,
                    BTOR_REAL_ADDR_NODE (
                        BTOR_REAL_ADDR_NODE (BTOR_REAL_ADDR_NODE (e0)->e[0])
                            ->e[0]),
                    BTOR_REAL_ADDR_NODE (
                        BTOR_REAL_ADDR_NODE (BTOR_REAL_ADDR_NODE (e0)->e[0])
                            ->e[1]));
                BTOR_DEC_REC_RW_CALL (btor);
              }
            }
            else if (BTOR_IS_INVERTED_NODE (e0)
                     && real_e0->kind == BTOR_AND_NODE)
            { /*  a | b == 0  -->  a == 0 && b == 0 */
              if (btor->rec_rw_calls < BTOR_REC_RW_BOUND)
              {
                BTOR_INC_REC_RW_CALL (btor);
                left = btor_rewrite_eq_exp (
                    btor, BTOR_INVERT_NODE (real_e0->e[0]), e1);
                right = btor_rewrite_eq_exp (
                    btor, BTOR_INVERT_NODE (real_e0->e[1]), e1);
                result = btor_rewrite_and_exp (btor, left, right);
                BTOR_DEC_REC_RW_CALL (btor);
                btor_release_exp (btor, left);
                btor_release_exp (btor, right);
              }
            }
            break;
          case BTOR_SLL_NODE:
          case BTOR_SRL_NODE:
          case BTOR_UREM_NODE:
          case BTOR_ADD_NODE: result = btor_copy_exp (btor, e0); break;
          case BTOR_MUL_NODE:
          case BTOR_AND_NODE:
            result = btor_zero_exp (btor, real_e0->len);
            break;
          case BTOR_ULT_NODE: /* x < 0 */ result = btor_false_exp (btor); break;
          case BTOR_UDIV_NODE:
            result = btor_ones_exp (btor, real_e0->len);
            break;
          default: break;
        }
        break;
      case BTOR_SPECIAL_CONST_ONE_ONES:
        assert (real_e1->len == 1);
        if (kind == BTOR_AND_NODE || kind == BTOR_BEQ_NODE
            || kind == BTOR_MUL_NODE || kind == BTOR_UDIV_NODE)
          result = btor_copy_exp (btor, e0);
        break;
      case BTOR_SPECIAL_CONST_ONE:
        if (kind == BTOR_MUL_NODE || kind == BTOR_UDIV_NODE)
          result = btor_copy_exp (btor, e0);
        else if (kind == BTOR_UREM_NODE)
          result = btor_zero_exp (btor, real_e0->len);
        else if (kind == BTOR_ULT_NODE)
        {
          tmp1   = btor_zero_exp (btor, real_e0->len);
          result = btor_rewrite_eq_exp (btor, e0, tmp1);
          btor_release_exp (btor, tmp1);
        }
        break;
      case BTOR_SPECIAL_CONST_ONES:
        if (kind == BTOR_BEQ_NODE)
        {
          if (is_xnor_exp (btor, e0)) /* (a XNOR b) == 1 -->  a = b */
          {
            if (btor->rec_rw_calls < BTOR_REC_RW_BOUND)
            {
              BTOR_INC_REC_RW_CALL (btor);
              result = btor_rewrite_eq_exp (
                  btor,
                  BTOR_REAL_ADDR_NODE (
                      BTOR_REAL_ADDR_NODE (BTOR_REAL_ADDR_NODE (e0)->e[0])
                          ->e[0]),
                  BTOR_REAL_ADDR_NODE (
                      BTOR_REAL_ADDR_NODE (BTOR_REAL_ADDR_NODE (e0)->e[0])
                          ->e[1]));
              BTOR_DEC_REC_RW_CALL (btor);
            }
          }
          else if (!BTOR_IS_INVERTED_NODE (e0) && e0->kind == BTOR_AND_NODE)
          { /* a & b == 1+ -->  a == 1+ && b == 1+ */
            if (btor->rec_rw_calls < BTOR_REC_RW_BOUND)
            {
              BTOR_INC_REC_RW_CALL (btor);
              left   = btor_rewrite_eq_exp (btor, e0->e[0], e1);
              right  = btor_rewrite_eq_exp (btor, e0->e[1], e1);
              result = btor_rewrite_and_exp (btor, left, right);
              BTOR_DEC_REC_RW_CALL (btor);
              btor_release_exp (btor, left);
              btor_release_exp (btor, right);
            }
          }
        }
        else if (kind == BTOR_AND_NODE)
          result = btor_copy_exp (btor, e0);
        else if (kind == BTOR_ULT_NODE)
          result = BTOR_INVERT_NODE (btor_rewrite_eq_exp (btor, e0, e1));
        break;
      default:
        assert (sc == BTOR_SPECIAL_CONST_NONE);
        if (kind == BTOR_BEQ_NODE && real_e0->kind == BTOR_AND_NODE
            && btor->rec_rw_calls < BTOR_REC_RW_BOUND)
        {
          BTOR_INC_REC_RW_CALL (btor);
          BTOR_INIT_STACK (stack);
          if (BTOR_IS_INVERTED_NODE (e1))
            bv_const = btor_not_const (btor->mm, real_e1->bits);
          else
            bv_const = btor_copy_const (btor->mm, real_e1->bits);

          pos = 0;
          /* a | b == const */
          if (BTOR_IS_INVERTED_NODE (e0))
          {
            while (pos < real_e1->len)
            {
              tmpString[0] = bv_const[pos];
              len          = (int) strspn (bv_const + pos, tmpString);
              tmp1         = btor_rewrite_slice_exp (btor,
                                             BTOR_INVERT_NODE (real_e0->e[0]),
                                             real_e1->len - 1 - pos,
                                             real_e1->len - pos - len);
              tmp2         = btor_rewrite_slice_exp (btor,
                                             BTOR_INVERT_NODE (real_e0->e[1]),
                                             real_e1->len - 1 - pos,
                                             real_e1->len - pos - len);
              if (tmpString[0] == '0')
              {
                tmp3 = btor_zero_exp (btor, len);
                BTOR_PUSH_STACK (
                    btor->mm, stack, btor_rewrite_eq_exp (btor, tmp1, tmp3));
                BTOR_PUSH_STACK (
                    btor->mm, stack, btor_rewrite_eq_exp (btor, tmp2, tmp3));
                btor_release_exp (btor, tmp3);
              }
              else
              {
                assert (tmpString[0] == '1');
                tmp3 = btor_or_exp (btor, tmp1, tmp2);
                tmp4 = btor_ones_exp (btor, len);
                BTOR_PUSH_STACK (
                    btor->mm, stack, btor_rewrite_eq_exp (btor, tmp3, tmp4));
                btor_release_exp (btor, tmp3);
                btor_release_exp (btor, tmp4);
              }
              btor_release_exp (btor, tmp1);
              btor_release_exp (btor, tmp2);
              pos += len;
            }
          }
          else
          {
            assert (!BTOR_IS_INVERTED_NODE (e0));
            /* a & b == const */
            while (pos < real_e1->len)
            {
              tmpString[0] = bv_const[pos];
              len          = (int) strspn (bv_const + pos, tmpString);
              tmp1         = btor_rewrite_slice_exp (btor,
                                             e0->e[0],
                                             real_e1->len - 1 - pos,
                                             real_e1->len - pos - len);
              tmp2         = btor_rewrite_slice_exp (btor,
                                             e0->e[1],
                                             real_e1->len - 1 - pos,
                                             real_e1->len - pos - len);
              if (tmpString[0] == '1')
              {
                tmp3 = btor_ones_exp (btor, len);
                BTOR_PUSH_STACK (
                    btor->mm, stack, btor_rewrite_eq_exp (btor, tmp1, tmp3));
                BTOR_PUSH_STACK (
                    btor->mm, stack, btor_rewrite_eq_exp (btor, tmp2, tmp3));
                btor_release_exp (btor, tmp3);
              }
              else
              {
                assert (tmpString[0] == '0');
                tmp3 = btor_rewrite_and_exp (btor, tmp1, tmp2);
                tmp4 = btor_zero_exp (btor, len);
                BTOR_PUSH_STACK (
                    btor->mm, stack, btor_rewrite_eq_exp (btor, tmp3, tmp4));
                btor_release_exp (btor, tmp3);
                btor_release_exp (btor, tmp4);
              }
              btor_release_exp (btor, tmp1);
              btor_release_exp (btor, tmp2);
              pos += len;
            }
          }

          result = btor_true_exp (btor);
          assert (!BTOR_EMPTY_STACK (stack));
          do
          {
            tmp1 = BTOR_POP_STACK (stack);
            tmp2 = btor_rewrite_and_exp (btor, result, tmp1);
            btor_release_exp (btor, result);
            result = tmp2;
            btor_release_exp (btor, tmp1);
          } while (!BTOR_EMPTY_STACK (stack));
          btor_delete_const (btor->mm, bv_const);
          BTOR_RELEASE_STACK (btor->mm, stack);
          BTOR_DEC_REC_RW_CALL (btor);
        }
        break;
    }
  }
  else if (real_e0 == real_e1
           && (kind == BTOR_BEQ_NODE || kind == BTOR_AEQ_NODE
               || kind == BTOR_ADD_NODE))
  {
    if (kind == BTOR_BEQ_NODE)
    {
      if (e0 == e1)
        result = btor_true_exp (btor); /* x == x */
      else
        result = btor_false_exp (btor); /* x == ~x */
    }
    else if (kind == BTOR_AEQ_NODE)
    {
      /* arrays must not be negated */
      assert (e0 == e1);
      result = btor_true_exp (btor); /* x == x */
    }
    else
    {
      assert (kind == BTOR_ADD_NODE);
      /* replace x + x by x * 2 */
      if (e0 == e1)
      {
        if (real_e0->len >= 2)
        {
          tmp1   = btor_int_to_exp (btor, 2, real_e0->len);
          result = btor_rewrite_mul_exp (btor, e0, tmp1);
          btor_release_exp (btor, tmp1);
        }
      }
      else
        /* replace x + ~x by -1 */
        result = btor_ones_exp (btor, real_e0->len);
    }
  }
  else if (e0 == e1
           && (kind == BTOR_ULT_NODE || kind == BTOR_UREM_NODE
               || kind == BTOR_UDIV_NODE))
  {
    switch (kind)
    {
      case BTOR_ULT_NODE:
        result = btor_false_exp (btor);
        break;
        /* v / v is 1 if v != 0 and UINT_MAX otherwise */
      case BTOR_UDIV_NODE:
        tmp2   = btor_zero_exp (btor, real_e0->len);
        tmp3   = btor_one_exp (btor, real_e0->len);
        tmp4   = btor_ones_exp (btor, real_e0->len);
        eq     = btor_rewrite_eq_exp (btor, e0, tmp2);
        result = btor_rewrite_cond_exp (btor, eq, tmp4, tmp3);
        btor_release_exp (btor, tmp2);
        btor_release_exp (btor, eq);
        btor_release_exp (btor, tmp4);
        btor_release_exp (btor, tmp3);
        break;
      default:
        assert (kind == BTOR_UREM_NODE);
        result = btor_zero_exp (btor, real_e0->len);
        break;
    }
  }
  else if (BTOR_IS_ARRAY_OR_BV_COND_NODE (real_e0)
           && BTOR_IS_ARRAY_OR_BV_COND_NODE (real_e1)
           && BTOR_IS_INVERTED_NODE (e0) == BTOR_IS_INVERTED_NODE (e1)
           && real_e0->e[0] == real_e1->e[0]
           && (real_e0->e[1] == real_e1->e[1] || real_e0->e[2] == real_e1->e[2])
           && (kind == BTOR_ULT_NODE || kind == BTOR_BEQ_NODE
               || kind == BTOR_AEQ_NODE || kind == BTOR_ADD_NODE
               || kind == BTOR_UDIV_NODE))
  {
    switch (kind)
    {
      case BTOR_ULT_NODE: fptr = btor_rewrite_ult_exp; break;
      case BTOR_BEQ_NODE:
      case BTOR_AEQ_NODE: fptr = btor_rewrite_eq_exp; break;
      case BTOR_ADD_NODE: fptr = btor_rewrite_add_exp; break;
      default:
        assert (kind == BTOR_UDIV_NODE);
        fptr = btor_rewrite_udiv_exp;
        break;
    }
    left   = fptr (btor,
                 BTOR_COND_INVERT_NODE (e0, real_e0->e[1]),
                 BTOR_COND_INVERT_NODE (e0, real_e1->e[1]));
    right  = fptr (btor,
                  BTOR_COND_INVERT_NODE (e0, real_e0->e[2]),
                  BTOR_COND_INVERT_NODE (e0, real_e1->e[2]));
    result = btor_rewrite_cond_exp (btor, real_e0->e[0], left, right);
    btor_release_exp (btor, left);
    btor_release_exp (btor, right);
  }

  /* TODO: lots of word level simplifications:
   * a[7:4] == b[7:4] && a[3:0] == b[3:0] <=> a == b
   * {a,b} == {c,d} with |a|=|c| <=> a == c && b == d
   * ...
   */
  /* TODO a + 2 * a <=> 3 * a <=> and see below */
  /* TODO strength reduction: a * 2 == a << 1 (really ?) */
  /* TODO strength reduction: a * 3 == (a << 1) + a (really ?) */
  /* TODO strength reduction: a / 2 == (a >> 1) (yes!) */
  /* TODO strength reduction: a / 3 =>  higher bits zero (check!) */
  /* TODO MAX-1 < a <=> a == MAX */

  /* TODO (x < ~x) <=> !msb(x) */
  /* TODO (~x < x) <=> msb(x) */

  /* TODO to support GAUSS bubble up odd terms:
   * (2 * a + 3 * y) + 4 * x => 3 * y + (2 * a + 4 * x)
   * or alternatively normalize arithmetic terms/polynomials
   * or simply always replace by equation.
   */

  /* TODO simplify (c * x + 2 * y) + x == 5 at GAUSS application
   * by first (c + 1) * x + 2 * y == 5 and then check whether 'c'
   * is even.
   */

  /* TODO Howto handle 2 * x == 4 && 4 * x + 8 * y == 0 ?
   * Maybe: x[30:0] == 2 && 4 * {x[31],2[30:0]} + 8 * y == 0?
   * Then: x[30:0] == 2 && 8[31:0] + 8 *y == 0?
   * Then: x[30:0] = 2 && 8 * y = -8
   * Finally:  x[30:0] = 2 && y[29:0] = -1
   * etc.
   */
  return result;
}

static int
is_always_unequal (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *real_e0, *real_e1;

  e0 = btor_pointer_chase_simplified_exp (btor, e0);
  e1 = btor_pointer_chase_simplified_exp (btor, e1);
  assert (btor);
  assert (e0);
  assert (e1);
  /* we need this so that a + 0 is rewritten to a,
   * and constants are normalized (all inverted constants are odd) */
  assert (btor->rewrite_level > 0);

  real_e0 = BTOR_REAL_ADDR_NODE (e0);
  real_e1 = BTOR_REAL_ADDR_NODE (e1);

  if (!real_e0 || !real_e1) return 0;

  if (BTOR_IS_ARRAY_NODE (real_e0))
  {
    assert (BTOR_IS_ARRAY_NODE (real_e1));
    return 0;
  }

  assert (!BTOR_IS_ARRAY_NODE (real_e1));

  if (e0 == BTOR_INVERT_NODE (e1)) return 1;

  if (BTOR_IS_BV_CONST_NODE (real_e0) && BTOR_IS_BV_CONST_NODE (real_e1)
      && e0 != e1)
    return 1;

  if (real_e0->kind == BTOR_ADD_NODE)
  {
    if (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (real_e0->e[0]))
        && !is_const_zero_exp (btor, real_e0->e[0])
        && BTOR_COND_INVERT_NODE (e0, real_e0->e[1]) == e1)
      return 1;
    if (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (real_e0->e[1]))
        && !is_const_zero_exp (btor, real_e0->e[1])
        && BTOR_COND_INVERT_NODE (e0, real_e0->e[0]) == e1)
      return 1;
  }

  if (real_e1->kind == BTOR_ADD_NODE)
  {
    if (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (real_e1->e[0]))
        && !is_const_zero_exp (btor, real_e1->e[0])
        && BTOR_COND_INVERT_NODE (e1, real_e1->e[1]) == e0)
      return 1;
    if (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (real_e1->e[1]))
        && !is_const_zero_exp (btor, real_e1->e[1])
        && BTOR_COND_INVERT_NODE (e1, real_e1->e[0]) == e0)
      return 1;
  }

  return 0;
}

static void
normalize_binary_comm_ass_exp (Btor *btor,
                               BtorNode *e0,
                               BtorNode *e1,
                               BtorNode **e0_norm,
                               BtorNode **e1_norm,
                               BtorNode *(*fptr) (Btor *,
                                                  BtorNode *,
                                                  BtorNode *),
                               BtorNodeKind kind)
{
  BtorMemMgr *mm;
  BtorNodePtrStack stack;
  BtorNode *cur, *result, *temp, *common;
  int i;
  BtorPtrHashTable *left, *right, *comm;
  BtorPtrHashBucket *b;
  assert (btor);
  assert (e0);
  assert (e1);
  assert (!BTOR_REAL_ADDR_NODE (e0)->simplified);
  assert (!BTOR_REAL_ADDR_NODE (e1)->simplified);
  assert (e0_norm);
  assert (e1_norm);
  assert (fptr);
  assert (BTOR_IS_BINARY_NODE_KIND (kind));
  assert (!BTOR_IS_INVERTED_NODE (e0));
  assert (!BTOR_IS_INVERTED_NODE (e1));
  assert (e0->kind == kind);
  assert (e1->kind == kind);
  assert (btor->rewrite_level > 2);

  mm    = btor->mm;
  left  = btor_new_ptr_hash_table (mm,
                                  (BtorHashPtr) btor_hash_exp_by_id,
                                  (BtorCmpPtr) btor_compare_exp_by_id);
  right = btor_new_ptr_hash_table (mm,
                                   (BtorHashPtr) btor_hash_exp_by_id,
                                   (BtorCmpPtr) btor_compare_exp_by_id);
  comm  = btor_new_ptr_hash_table (mm,
                                  (BtorHashPtr) btor_hash_exp_by_id,
                                  (BtorCmpPtr) btor_compare_exp_by_id);

  BTOR_INIT_STACK (stack);
  BTOR_PUSH_STACK (mm, stack, e0);
  do
  {
    cur = BTOR_POP_STACK (stack);
    if (!BTOR_IS_INVERTED_NODE (cur) && cur->kind == kind)
    {
      BTOR_PUSH_STACK (mm, stack, cur->e[1]);
      BTOR_PUSH_STACK (mm, stack, cur->e[0]);
    }
    else
    {
      b = btor_find_in_ptr_hash_table (left, cur);
      if (!b)
        btor_insert_in_ptr_hash_table (left, cur)->data.asInt = 1;
      else
        b->data.asInt++;
    }
  } while (!BTOR_EMPTY_STACK (stack));

  BTOR_PUSH_STACK (mm, stack, e1);
  do
  {
    cur = BTOR_POP_STACK (stack);
    if (!BTOR_IS_INVERTED_NODE (cur) && cur->kind == kind)
    {
      BTOR_PUSH_STACK (mm, stack, cur->e[1]);
      BTOR_PUSH_STACK (mm, stack, cur->e[0]);
    }
    else
    {
      b = btor_find_in_ptr_hash_table (left, cur);
      if (b)
      {
        /* we found one common operand */

        /* remove operand from left */
        if (b->data.asInt > 1)
          b->data.asInt--;
        else
        {
          assert (b->data.asInt == 1);
          btor_remove_from_ptr_hash_table (left, cur, 0, 0);
        }

        /* insert into common table */
        b = btor_find_in_ptr_hash_table (comm, cur);
        if (!b)
          btor_insert_in_ptr_hash_table (comm, cur)->data.asInt = 1;
        else
          b->data.asInt++;
      }
      else
      {
        /* operand is not common */
        b = btor_find_in_ptr_hash_table (right, cur);
        if (!b)
          btor_insert_in_ptr_hash_table (right, cur)->data.asInt = 1;
        else
          b->data.asInt++;
      }
    }
  } while (!BTOR_EMPTY_STACK (stack));
  BTOR_RELEASE_STACK (mm, stack);

  /* no operand or only one operand in common? leave everything as it is */
  if (comm->count < 2u)
  {
    /* clean up */
    btor_delete_ptr_hash_table (left);
    btor_delete_ptr_hash_table (right);
    btor_delete_ptr_hash_table (comm);
    *e0_norm = btor_copy_exp (btor, e0);
    *e1_norm = btor_copy_exp (btor, e1);
    return;
  }

  if (kind == BTOR_ADD_NODE)
    btor->stats.adds_normalized++;
  else
  {
    assert (kind == BTOR_MUL_NODE);
    btor->stats.muls_normalized++;
  }

  assert (comm->count >= 2u);
  b      = comm->first;
  common = btor_copy_exp (btor, (BtorNode *) b->key);
  if (b->data.asInt > 0)
    b->data.asInt--;
  else
    b = b->next;
  while (b)
  {
    cur = b->key;
    for (i = 0; i < b->data.asInt; i++)
    {
      temp = fptr (btor, common, cur);
      btor_release_exp (btor, common);
      common = temp;
    }
    b = b->next;
  }

  /* normalize left side */
  result = btor_copy_exp (btor, common);
  for (b = left->first; b; b = b->next)
  {
    cur = b->key;
    for (i = 0; i < b->data.asInt; i++)
    {
      temp = fptr (btor, result, cur);
      btor_release_exp (btor, result);
      result = temp;
    }
  }
  *e0_norm = result;

  /* normalize right side */
  result = btor_copy_exp (btor, common);
  for (b = right->first; b; b = b->next)
  {
    cur = b->key;
    for (i = 0; i < b->data.asInt; i++)
    {
      temp = fptr (btor, result, cur);
      btor_release_exp (btor, result);
      result = temp;
    }
  }
  *e1_norm = result;

  /* clean up */
  btor_release_exp (btor, common);
  btor_delete_ptr_hash_table (left);
  btor_delete_ptr_hash_table (right);
  btor_delete_ptr_hash_table (comm);
}

static void
normalize_adds_exp (Btor *btor,
                    BtorNode *e0,
                    BtorNode *e1,
                    BtorNode **e0_norm,
                    BtorNode **e1_norm)
{
  assert (btor);
  assert (e0);
  assert (e1);
  assert (!BTOR_REAL_ADDR_NODE (e0)->simplified);
  assert (!BTOR_REAL_ADDR_NODE (e1)->simplified);
  assert (e0_norm);
  assert (e1_norm);
  assert (!BTOR_IS_INVERTED_NODE (e0));
  assert (!BTOR_IS_INVERTED_NODE (e1));
  assert (e0->kind == BTOR_ADD_NODE);
  assert (e1->kind == BTOR_ADD_NODE);
  normalize_binary_comm_ass_exp (
      btor, e0, e1, e0_norm, e1_norm, btor_rewrite_add_exp, BTOR_ADD_NODE);
}

static void
normalize_muls_exp (Btor *btor,
                    BtorNode *e0,
                    BtorNode *e1,
                    BtorNode **e0_norm,
                    BtorNode **e1_norm)
{
  assert (btor);
  assert (e0);
  assert (e1);
  assert (!BTOR_REAL_ADDR_NODE (e0)->simplified);
  assert (!BTOR_REAL_ADDR_NODE (e1)->simplified);
  assert (e0_norm);
  assert (e1_norm);
  assert (!BTOR_IS_INVERTED_NODE (e0));
  assert (!BTOR_IS_INVERTED_NODE (e1));
  assert (e0->kind == BTOR_MUL_NODE);
  assert (e1->kind == BTOR_MUL_NODE);
  normalize_binary_comm_ass_exp (
      btor, e0, e1, e0_norm, e1_norm, btor_rewrite_mul_exp, BTOR_MUL_NODE);
}

static int
find_and_contradiction_exp (
    Btor *btor, BtorNode *exp, BtorNode *e0, BtorNode *e1, int *calls)
{
  assert (btor);
  assert (exp);
  assert (e0);
  assert (e1);
  assert (calls);
  assert (*calls >= 0);
  (void) btor;

  if (*calls >= BTOR_FIND_AND_NODE_CONTRADICTION_LIMIT) return 0;

  if (!BTOR_IS_INVERTED_NODE (exp) && exp->kind == BTOR_AND_NODE)
  {
    if (exp->e[0] == BTOR_INVERT_NODE (e0) || exp->e[0] == BTOR_INVERT_NODE (e1)
        || exp->e[1] == BTOR_INVERT_NODE (e0)
        || exp->e[1] == BTOR_INVERT_NODE (e1))
      return 1;
    *calls += 1;
    return find_and_contradiction_exp (btor, exp->e[0], e0, e1, calls)
           || find_and_contradiction_exp (btor, exp->e[1], e0, e1, calls);
  }
  return 0;
}

BtorNode *
btor_rewrite_and_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *real_e0, *real_e1, *result, *e0_norm, *e1_norm, *temp;
  int normalized, calls;

  normalized = 0;
  calls      = 0;

BTOR_NODE_TWO_LEVEL_OPT_TRY_AGAIN:
  /* two level optimization [MEMICS] for BTOR_AND_NODE */
  assert (!normalized);
  e0 = btor_pointer_chase_simplified_exp (btor, e0);
  e1 = btor_pointer_chase_simplified_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));

  real_e0 = BTOR_REAL_ADDR_NODE (e0);
  real_e1 = BTOR_REAL_ADDR_NODE (e1);

  if (e0 == e1) /* x & x == x */
  {
    return btor_copy_exp (btor, e0);
  }

  if (BTOR_INVERT_NODE (e0) == e1) /* x & ~x == 0 */
  {
    return btor_zero_exp (btor, real_e0->len);
  }

  if (real_e0->kind == BTOR_AND_NODE && real_e1->kind == BTOR_AND_NODE)
  {
    if (!BTOR_IS_INVERTED_NODE (e0) && !BTOR_IS_INVERTED_NODE (e1))
    {
      /* second rule of contradiction */
      if (real_e0->e[0] == BTOR_INVERT_NODE (real_e1->e[0])
          || real_e0->e[0] == BTOR_INVERT_NODE (real_e1->e[1])
          || real_e0->e[1] == BTOR_INVERT_NODE (real_e1->e[0])
          || real_e0->e[1] == BTOR_INVERT_NODE (real_e1->e[1]))
      {
        return btor_zero_exp (btor, real_e0->len);
      }
      /* symmetric rule of idempotency */
      if (real_e0->e[0] == real_e1->e[0] || real_e0->e[1] == real_e1->e[0])
      {
        e1 = real_e1->e[1];
        goto BTOR_NODE_TWO_LEVEL_OPT_TRY_AGAIN;
      }
      /* use commutativity */
      if (real_e0->e[0] == real_e1->e[1] || real_e0->e[1] == real_e1->e[1])
      {
        e1 = real_e1->e[0];
        goto BTOR_NODE_TWO_LEVEL_OPT_TRY_AGAIN;
      }
    }
    else if (!BTOR_IS_INVERTED_NODE (e0) && BTOR_IS_INVERTED_NODE (e1))
    {
      /* second rule of subsumption */
      if (real_e0->e[0] == BTOR_INVERT_NODE (real_e1->e[0])
          || real_e0->e[0] == BTOR_INVERT_NODE (real_e1->e[1])
          || real_e0->e[1] == BTOR_INVERT_NODE (real_e1->e[0])
          || real_e0->e[1] == BTOR_INVERT_NODE (real_e1->e[1]))
      {
        return btor_copy_exp (btor, e0);
      }
      /* symmetric rule of substitution */
      if ((real_e1->e[0] == real_e0->e[1]) || (real_e1->e[0] == real_e0->e[0]))
      {
        e1 = BTOR_INVERT_NODE (real_e1->e[1]);
        goto BTOR_NODE_TWO_LEVEL_OPT_TRY_AGAIN;
      }
      /* symmetric rule of substitution */
      if ((real_e1->e[1] == real_e0->e[1]) || (real_e1->e[1] == real_e0->e[0]))
      {
        e1 = BTOR_INVERT_NODE (real_e1->e[0]);
        goto BTOR_NODE_TWO_LEVEL_OPT_TRY_AGAIN;
      }
    }
    else if (BTOR_IS_INVERTED_NODE (e0) && !BTOR_IS_INVERTED_NODE (e1))
    {
      /* second rule of subsumption */
      if (real_e0->e[0] == BTOR_INVERT_NODE (real_e1->e[0])
          || real_e0->e[0] == BTOR_INVERT_NODE (real_e1->e[1])
          || real_e0->e[1] == BTOR_INVERT_NODE (real_e1->e[0])
          || real_e0->e[1] == BTOR_INVERT_NODE (real_e1->e[1]))
      {
        return btor_copy_exp (btor, e1);
      }
      /* symmetric rule of substitution */
      if ((real_e0->e[1] == real_e1->e[0]) || (real_e0->e[1] == real_e1->e[1]))
      {
        e0 = BTOR_INVERT_NODE (real_e0->e[0]);
        goto BTOR_NODE_TWO_LEVEL_OPT_TRY_AGAIN;
      }
      /* symmetric rule of substitution */
      if ((real_e0->e[0] == real_e1->e[0]) || (real_e0->e[0] == real_e1->e[1]))
      {
        e0 = BTOR_INVERT_NODE (real_e0->e[1]);
        goto BTOR_NODE_TWO_LEVEL_OPT_TRY_AGAIN;
      }
    }
    else
    {
      assert (BTOR_IS_INVERTED_NODE (e0));
      assert (BTOR_IS_INVERTED_NODE (e1));
      /* a XNOR b simplifies to a == b for the boolean case */
      if (real_e0->len == 1
          && BTOR_IS_INVERTED_NODE (real_e0->e[0])
                 != BTOR_IS_INVERTED_NODE (real_e0->e[1])
          && BTOR_IS_INVERTED_NODE (real_e1->e[0])
                 != BTOR_IS_INVERTED_NODE (real_e1->e[1])
          && ((real_e0->e[0] == BTOR_INVERT_NODE (real_e1->e[0])
               && real_e0->e[1] == BTOR_INVERT_NODE (real_e1->e[1]))
              || (real_e0->e[0] == BTOR_INVERT_NODE (real_e1->e[1])
                  && real_e0->e[1] == BTOR_INVERT_NODE (real_e1->e[0]))))
      {
        if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
          goto BTOR_REWRITE_AND_NODE_NO_REWRITE;
        BTOR_INC_REC_RW_CALL (btor);
        result = btor_rewrite_eq_exp (btor,
                                      BTOR_REAL_ADDR_NODE (real_e0->e[0]),
                                      BTOR_REAL_ADDR_NODE (real_e0->e[1]));
        BTOR_DEC_REC_RW_CALL (btor);
        return result;
      }
      /* rule of resolution */
      if ((real_e0->e[0] == real_e1->e[0]
           && real_e0->e[1] == BTOR_INVERT_NODE (real_e1->e[1]))
          || (real_e0->e[0] == real_e1->e[1]
              && real_e0->e[1] == BTOR_INVERT_NODE (real_e1->e[0])))
      {
        return BTOR_INVERT_NODE (btor_copy_exp (btor, real_e0->e[0]));
      }
      /* rule of resolution */
      if ((real_e1->e[1] == real_e0->e[1]
           && real_e1->e[0] == BTOR_INVERT_NODE (real_e0->e[0]))
          || (real_e1->e[1] == real_e0->e[0]
              && real_e1->e[0] == BTOR_INVERT_NODE (real_e0->e[1])))
      {
        return BTOR_INVERT_NODE (btor_copy_exp (btor, real_e1->e[1]));
      }
    }
  }

  if (real_e0->kind == BTOR_AND_NODE)
  {
    if (BTOR_IS_INVERTED_NODE (e0))
    {
      /* first rule of subsumption */
      if (real_e0->e[0] == BTOR_INVERT_NODE (e1)
          || real_e0->e[1] == BTOR_INVERT_NODE (e1))
      {
        return btor_copy_exp (btor, e1);
      }

      /* asymmetric rule of substitution */
      if (real_e0->e[1] == e1)
      {
        e0 = BTOR_INVERT_NODE (real_e0->e[0]);
        goto BTOR_NODE_TWO_LEVEL_OPT_TRY_AGAIN;
      }

      /* asymmetric rule of substitution */
      if (real_e0->e[0] == e1)
      {
        e0 = BTOR_INVERT_NODE (real_e0->e[1]);
        goto BTOR_NODE_TWO_LEVEL_OPT_TRY_AGAIN;
      }
    }
    else
    {
      assert (!BTOR_IS_INVERTED_NODE (e0));

      /* first rule of contradiction */
      if (e0->e[0] == BTOR_INVERT_NODE (e1)
          || e0->e[1] == BTOR_INVERT_NODE (e1))
      {
        return btor_zero_exp (btor, e0->len);
      }

      /* asymmetric rule of idempotency */
      if (e0->e[0] == e1 || e0->e[1] == e1)
      {
        return btor_copy_exp (btor, e0);
      }

      if (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e1)))
      {
        if (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e0->e[0])))
        {
          assert (!BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e0->e[1])));
          if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
            goto BTOR_REWRITE_AND_NODE_NO_REWRITE;
          BTOR_INC_REC_RW_CALL (btor);
          temp   = btor_rewrite_and_exp (btor, e1, e0->e[0]);
          result = btor_rewrite_and_exp (btor, temp, e0->e[1]);
          BTOR_DEC_REC_RW_CALL (btor);
          btor_release_exp (btor, temp);
          return result;
        }

        if (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e0->e[1])))
        {
          assert (!BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e0->e[0])));
          if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
            goto BTOR_REWRITE_AND_NODE_NO_REWRITE;
          BTOR_INC_REC_RW_CALL (btor);
          temp   = btor_rewrite_and_exp (btor, e1, e0->e[1]);
          result = btor_rewrite_and_exp (btor, temp, e0->e[0]);
          BTOR_DEC_REC_RW_CALL (btor);
          btor_release_exp (btor, temp);
          return result;
        }
      }
    }
  }

  if (real_e1->kind == BTOR_AND_NODE)
  {
    if (BTOR_IS_INVERTED_NODE (e1))
    {
      /* first rule of subsumption */
      if (real_e1->e[0] == BTOR_INVERT_NODE (e0)
          || real_e1->e[1] == BTOR_INVERT_NODE (e0))
      {
        return btor_copy_exp (btor, e0);
      }
      /* asymmetric rule of substitution */
      if (real_e1->e[0] == e0)
      {
        e1 = BTOR_INVERT_NODE (real_e1->e[1]);
        goto BTOR_NODE_TWO_LEVEL_OPT_TRY_AGAIN;
      }
      /* asymmetric rule of substitution */
      if (real_e1->e[1] == e0)
      {
        e1 = BTOR_INVERT_NODE (real_e1->e[0]);
        goto BTOR_NODE_TWO_LEVEL_OPT_TRY_AGAIN;
      }
    }
    else
    {
      assert (!BTOR_IS_INVERTED_NODE (e1));

      /* first rule of contradiction */
      if (e1->e[0] == BTOR_INVERT_NODE (e0)
          || e1->e[1] == BTOR_INVERT_NODE (e0))
      {
        return btor_zero_exp (btor, real_e0->len);
      }

      /* asymmetric rule of idempotency */
      if (e1->e[0] == e0 || e1->e[1] == e0)
      {
        return btor_copy_exp (btor, e1);
      }

      if (BTOR_IS_BV_CONST_NODE (real_e0))
      {
        /* recursion is no problem here, as one call leads to
         * folding of constants, and the other call can not
         * trigger the same kind of recursion anymore */

        if (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e1->e[0])))
        {
          assert (!BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e1->e[1])));
          if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
            goto BTOR_REWRITE_AND_NODE_NO_REWRITE;
          BTOR_INC_REC_RW_CALL (btor);
          temp   = btor_rewrite_and_exp (btor, e0, e1->e[0]);
          result = btor_rewrite_and_exp (btor, temp, e1->e[1]);
          BTOR_DEC_REC_RW_CALL (btor);
          btor_release_exp (btor, temp);
          return result;
        }

        if (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e1->e[1])))
        {
          assert (!BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e1->e[0])));
          if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
            goto BTOR_REWRITE_AND_NODE_NO_REWRITE;
          BTOR_INC_REC_RW_CALL (btor);
          temp   = btor_rewrite_and_exp (btor, e0, e1->e[1]);
          result = btor_rewrite_and_exp (btor, temp, e1->e[0]);
          BTOR_DEC_REC_RW_CALL (btor);
          btor_release_exp (btor, temp);
          return result;
        }
      }
    }
  }

  if (real_e0->kind == BTOR_ULT_NODE && real_e1->kind == BTOR_ULT_NODE)
  {
    /* a < b && b < a simplifies to FALSE */
    if (!BTOR_IS_INVERTED_NODE (e0) && !BTOR_IS_INVERTED_NODE (e1)
        && e0->e[0] == e1->e[1] && e0->e[1] == e1->e[0])
    {
      return btor_false_exp (btor);
    }
    /* NOT (a < b) && NOT (b < a) simplifies to a == b */
    if (BTOR_IS_INVERTED_NODE (e0) && BTOR_IS_INVERTED_NODE (e1)
        && real_e0->e[0] == real_e1->e[1] && real_e0->e[1] == real_e1->e[0])
    {
      if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
        goto BTOR_REWRITE_AND_NODE_NO_REWRITE;
      BTOR_INC_REC_RW_CALL (btor);
      result = btor_rewrite_eq_exp (btor, real_e0->e[0], real_e0->e[1]);
      BTOR_DEC_REC_RW_CALL (btor);
      return result;
    }
  }

  if (find_and_contradiction_exp (btor, e0, e0, e1, &calls)
      || find_and_contradiction_exp (btor, e1, e0, e1, &calls))
    return btor_zero_exp (btor, real_e0->len);

  if (btor->rewrite_level > 2 && !BTOR_IS_INVERTED_NODE (e0)
      && !BTOR_IS_INVERTED_NODE (e1) && e0->kind == e1->kind)
  {
    if (e0->kind == BTOR_ADD_NODE)
    {
      assert (e1->kind == BTOR_ADD_NODE);
      normalize_adds_exp (btor, e0, e1, &e0_norm, &e1_norm);
      normalized = 1;
      e0         = e0_norm;
      e1         = e1_norm;
    }
    else if (e0->kind == BTOR_MUL_NODE)
    {
      assert (e1->kind == BTOR_MUL_NODE);
      normalize_muls_exp (btor, e0, e1, &e0_norm, &e1_norm);
      normalized = 1;
      e0         = e0_norm;
      e1         = e1_norm;
    }
  }

  result = rewrite_binary_exp (btor, BTOR_AND_NODE, e0, e1);

  if (!result)
  {
  BTOR_REWRITE_AND_NODE_NO_REWRITE:
    result = btor_and_exp_node (btor, e0, e1);
  }

  if (normalized)
  {
    btor_release_exp (btor, e0_norm);
    btor_release_exp (btor, e1_norm);
  }

  return result;
}

/* This function tries to rewrite a * b + a * c into a * (b + c)
 * it returns 0 when it has not succeeded */
static BtorNode *
try_rewrite_add_mul_distrib (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *result, *add, *mul;

  assert (btor);
  assert (e0);
  assert (e1);
  assert (btor->rewrite_level > 2);

  result = 0;
  add    = 0;
  mul    = 0;

  if (!BTOR_IS_INVERTED_NODE (e0) && !BTOR_IS_INVERTED_NODE (e1)
      && e0->kind == BTOR_MUL_NODE && e1->kind == BTOR_MUL_NODE)
  {
    if (e0->e[0] == e1->e[0])
    {
      add = btor_rewrite_add_exp (btor, e0->e[1], e1->e[1]);
      mul = e0->e[0];
    }
    else if (e0->e[0] == e1->e[1])
    {
      add = btor_rewrite_add_exp (btor, e0->e[1], e1->e[0]);
      mul = e0->e[0];
    }
    else if (e0->e[1] == e1->e[0])
    {
      add = btor_rewrite_add_exp (btor, e0->e[0], e1->e[1]);
      mul = e0->e[1];
    }
    else if (e0->e[1] == e1->e[1])
    {
      add = btor_rewrite_add_exp (btor, e0->e[0], e1->e[0]);
      mul = e0->e[1];
    }

    if (add)
    {
      assert (mul);
      result = btor_rewrite_mul_exp (btor, mul, add);
      btor_release_exp (btor, add);
      /* mul owns no reference */
    }
  }

  return result;
}

BtorNode *
btor_rewrite_eq_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *tmp1, *tmp2, *result;
  BtorNode *tmp3, *tmp4;
  BtorNode *real_e0, *real_e1;
  int normalized;
  int upper, lower;
  BtorNodeKind kind;

  e0 = btor_pointer_chase_simplified_exp (btor, e0);
  e1 = btor_pointer_chase_simplified_exp (btor, e1);
  assert (btor_precond_eq_exp_dbg (btor, e0, e1));
  assert (btor->rewrite_level > 0);

  normalized = 0;
  real_e0    = BTOR_REAL_ADDR_NODE (e0);
  real_e1    = BTOR_REAL_ADDR_NODE (e1);
  if (BTOR_IS_ARRAY_NODE (real_e0))
    kind = BTOR_AEQ_NODE;
  else
    kind = BTOR_BEQ_NODE;

  /* write (a, i, e1) = write (a, i, e2) ----> e1 = e2 */
  if (kind == BTOR_AEQ_NODE && BTOR_IS_WRITE_NODE (e0)
      && BTOR_IS_WRITE_NODE (e1) && e0->e[0] == e1->e[0]
      && e0->e[1] == e1->e[1])
  {
    e0   = e0->e[2];
    e1   = e1->e[2];
    kind = BTOR_BEQ_NODE;
  }

  /* ~e0 == ~e1 is the same as e0 == e1 */
  if (BTOR_IS_INVERTED_NODE (e0) && BTOR_IS_INVERTED_NODE (e1))
  {
    e0 = BTOR_REAL_ADDR_NODE (e0);
    e1 = BTOR_REAL_ADDR_NODE (e1);
  }

  /* we do not rewrite eq in the boolean case, as we
   * cannot extract the resulting XNOR on top level again and
   * would therefore lose substitutions
   * additionally, we do not rewrite eq in the boolean case, as
   * we rewrite a != b to a = ~b and substitute.
   * */

  if (e0 == e1)
  {
    return btor_true_exp (btor);
  }

  if (is_always_unequal (btor, e0, e1))
  {
    return btor_false_exp (btor);
  }

  if (btor->rewrite_level > 2)
  {
    if (!BTOR_IS_INVERTED_NODE (e0))
    {
      /* a + b = a ----> b = 0,
       * this rule does not lead to less substitutions. 'a' cannot
       * be substituted as the occurrence check would fail */
      if (e0->kind == BTOR_ADD_NODE)
      {
        if (e0->e[0] == e1)
        {
          if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
            goto BTOR_REWRITE_EQ_NODE_NO_REWRITE;
          tmp1 = btor_zero_exp (btor, e0->len);
          BTOR_INC_REC_RW_CALL (btor);
          result = btor_rewrite_eq_exp (btor, tmp1, e0->e[1]);
          BTOR_DEC_REC_RW_CALL (btor);
          btor_release_exp (btor, tmp1);
          return result;
        }

        if (e0->e[1] == e1)
        {
          if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
            goto BTOR_REWRITE_EQ_NODE_NO_REWRITE;
          tmp1 = btor_zero_exp (btor, e0->len);
          BTOR_INC_REC_RW_CALL (btor);
          result = btor_rewrite_eq_exp (btor, tmp1, e0->e[0]);
          btor_release_exp (btor, tmp1);
          BTOR_DEC_REC_RW_CALL (btor);
          return result;
        }
      }

      /* b ? a : t = d  ---a != d-->  !b AND d = t */
      if (e0->kind == BTOR_BCOND_NODE)
      {
        if (is_always_unequal (btor, e0->e[1], e1))
        {
          if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
            goto BTOR_REWRITE_EQ_NODE_NO_REWRITE;
          BTOR_INC_REC_RW_CALL (btor);
          tmp1 = btor_rewrite_eq_exp (btor, e0->e[2], e1);
          result =
              btor_rewrite_and_exp (btor, BTOR_INVERT_NODE (e0->e[0]), tmp1);
          BTOR_DEC_REC_RW_CALL (btor);
          btor_release_exp (btor, tmp1);
          return result;
        }

        if (is_always_unequal (btor, e0->e[2], e1))
        {
          if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
            goto BTOR_REWRITE_EQ_NODE_NO_REWRITE;
          BTOR_INC_REC_RW_CALL (btor);
          tmp1   = btor_rewrite_eq_exp (btor, e0->e[1], e1);
          result = btor_rewrite_and_exp (btor, e0->e[0], tmp1);
          BTOR_DEC_REC_RW_CALL (btor);
          btor_release_exp (btor, tmp1);
          return result;
        }
      }
    }

    if (!BTOR_IS_INVERTED_NODE (e1))
    {
      /* a = a + b  ----> b = 0,
       * this rule does not lead to less substitutions. 'a' cannot
       * be substituted as the occurrence check would fail */
      if (e1->kind == BTOR_ADD_NODE)
      {
        if (e1->e[0] == e0)
        {
          if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
            goto BTOR_REWRITE_EQ_NODE_NO_REWRITE;
          tmp1 = btor_zero_exp (btor, e1->len);
          BTOR_INC_REC_RW_CALL (btor);
          result = btor_rewrite_eq_exp (btor, tmp1, e1->e[1]);
          BTOR_DEC_REC_RW_CALL (btor);
          btor_release_exp (btor, tmp1);
          return result;
        }

        if (e1->e[1] == e0)
        {
          if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
            goto BTOR_REWRITE_EQ_NODE_NO_REWRITE;
          tmp1 = btor_zero_exp (btor, e1->len);
          BTOR_INC_REC_RW_CALL (btor);
          result = btor_rewrite_eq_exp (btor, tmp1, e1->e[0]);
          BTOR_DEC_REC_RW_CALL (btor);
          btor_release_exp (btor, tmp1);
          return result;
        }
      }

      /* d = b ? a : t  ---a != d-->  !b AND d = t */
      if (e1->kind == BTOR_BCOND_NODE)
      {
        if (is_always_unequal (btor, e1->e[1], e0))
        {
          if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
            goto BTOR_REWRITE_EQ_NODE_NO_REWRITE;
          BTOR_INC_REC_RW_CALL (btor);
          tmp1 = btor_rewrite_eq_exp (btor, e1->e[2], e0);
          result =
              btor_rewrite_and_exp (btor, BTOR_INVERT_NODE (e1->e[0]), tmp1);
          BTOR_DEC_REC_RW_CALL (btor);
          btor_release_exp (btor, tmp1);
          return result;
        }

        if (is_always_unequal (btor, e1->e[2], e0))
        {
          if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
            goto BTOR_REWRITE_EQ_NODE_NO_REWRITE;
          BTOR_INC_REC_RW_CALL (btor);
          tmp1   = btor_rewrite_eq_exp (btor, e1->e[1], e0);
          result = btor_rewrite_and_exp (btor, e1->e[0], tmp1);
          BTOR_DEC_REC_RW_CALL (btor);
          btor_release_exp (btor, tmp1);
          return result;
        }
      }
    }

    if (!BTOR_IS_INVERTED_NODE (e0) && !BTOR_IS_INVERTED_NODE (e1))
    {
      /* a + b = a + c ---> b = c */
      if (e0->kind == BTOR_ADD_NODE && e1->kind == BTOR_ADD_NODE)
      {
        if (e0->e[0] == e1->e[0])
        {
          if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
            goto BTOR_REWRITE_EQ_NODE_NO_REWRITE;
          BTOR_INC_REC_RW_CALL (btor);
          result = btor_rewrite_eq_exp (btor, e0->e[1], e1->e[1]);
          BTOR_DEC_REC_RW_CALL (btor);
          return result;
        }

        if (e0->e[0] == e1->e[1])
        {
          if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
            goto BTOR_REWRITE_EQ_NODE_NO_REWRITE;
          BTOR_INC_REC_RW_CALL (btor);
          result = btor_rewrite_eq_exp (btor, e0->e[1], e1->e[0]);
          BTOR_DEC_REC_RW_CALL (btor);
          return result;
        }

        if (e0->e[1] == e1->e[0])
        {
          if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
            goto BTOR_REWRITE_EQ_NODE_NO_REWRITE;
          BTOR_INC_REC_RW_CALL (btor);
          result = btor_rewrite_eq_exp (btor, e0->e[0], e1->e[1]);
          BTOR_DEC_REC_RW_CALL (btor);
          return result;
        }

        if (e0->e[1] == e1->e[1])
        {
          if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
            goto BTOR_REWRITE_EQ_NODE_NO_REWRITE;
          BTOR_INC_REC_RW_CALL (btor);
          result = btor_rewrite_eq_exp (btor, e0->e[0], e1->e[0]);
          BTOR_DEC_REC_RW_CALL (btor);
          return result;
        }
      }

      /* commutative operators are normalized ignoring sign, so
       * we do not have to check cases like a & b ==  ~b & a as they
       * are represented as a & b == a & ~b */

      if (e0->kind == BTOR_AND_NODE && e1->kind == BTOR_AND_NODE)
      {
        if (e0->e[0] == BTOR_INVERT_NODE (e1->e[0])
            && e0->e[1] == BTOR_INVERT_NODE (e1->e[1]))
        {
          /* a & b == ~a & ~b   -->  a == ~b */
          if (BTOR_IS_INVERTED_NODE (e0->e[0])
              == BTOR_IS_INVERTED_NODE (e0->e[1]))
          {
            assert (BTOR_IS_INVERTED_NODE (e1->e[0])
                    == BTOR_IS_INVERTED_NODE (e1->e[1]));
            if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
              goto BTOR_REWRITE_EQ_NODE_NO_REWRITE;
            BTOR_INC_REC_RW_CALL (btor);
            result = btor_rewrite_eq_exp (
                btor, e0->e[0], BTOR_INVERT_NODE (e0->e[1]));
            BTOR_DEC_REC_RW_CALL (btor);
            return result;
          }

          /* a~ & b == a & ~b   -->  a == b */
          if (BTOR_IS_INVERTED_NODE (e0->e[0])
              != BTOR_IS_INVERTED_NODE (e0->e[1]))
          {
            assert (BTOR_IS_INVERTED_NODE (e1->e[0])
                    != BTOR_IS_INVERTED_NODE (e1->e[1]));
            if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
              goto BTOR_REWRITE_EQ_NODE_NO_REWRITE;
            BTOR_INC_REC_RW_CALL (btor);
            result = btor_rewrite_eq_exp (btor,
                                          BTOR_REAL_ADDR_NODE (e0->e[0]),
                                          BTOR_REAL_ADDR_NODE (e0->e[1]));
            BTOR_DEC_REC_RW_CALL (btor);
            return result;
          }
        }
        /* a & b == a & ~b  ---> a == 0 */
        if (e0->e[0] == e1->e[0] && e0->e[1] == BTOR_INVERT_NODE (e1->e[1]))
        {
          if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
            goto BTOR_REWRITE_EQ_NODE_NO_REWRITE;
          tmp1 = btor_zero_exp (btor, e0->len);
          BTOR_INC_REC_RW_CALL (btor);
          result = btor_rewrite_eq_exp (btor, e0->e[0], tmp1);
          BTOR_DEC_REC_RW_CALL (btor);
          btor_release_exp (btor, tmp1);
          return result;
        }

        /* a & b == ~a & b ---> b == 0 */
        if (e0->e[1] == e1->e[1] && e0->e[0] == BTOR_INVERT_NODE (e1->e[0]))
        {
          if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
            goto BTOR_REWRITE_EQ_NODE_NO_REWRITE;
          tmp1 = btor_zero_exp (btor, e0->len);
          BTOR_INC_REC_RW_CALL (btor);
          result = btor_rewrite_eq_exp (btor, e0->e[1], tmp1);
          BTOR_DEC_REC_RW_CALL (btor);
          btor_release_exp (btor, tmp1);
          return result;
        }
      }
    }

    /* a = b ? a : c is rewritten to  b OR a = c
     * a = ~(b ? a : c) is rewritten to  !b AND a = ~c
     */
    if (BTOR_IS_ARRAY_OR_BV_COND_NODE (real_e0))
    {
      if (real_e0->e[1] == e1)
      {
        if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
          goto BTOR_REWRITE_EQ_NODE_NO_REWRITE;
        BTOR_INC_REC_RW_CALL (btor);
        if (BTOR_IS_INVERTED_NODE (e0))
        {
          tmp1 =
              btor_rewrite_eq_exp (btor, BTOR_INVERT_NODE (real_e0->e[2]), e1);
          result = btor_rewrite_and_exp (
              btor, BTOR_INVERT_NODE (real_e0->e[0]), tmp1);
        }
        else
        {
          tmp1   = btor_rewrite_eq_exp (btor, real_e0->e[2], e1);
          result = btor_or_exp (btor, real_e0->e[0], tmp1);
        }
        btor_release_exp (btor, tmp1);
        BTOR_DEC_REC_RW_CALL (btor);
        return result;
      }

      if (real_e0->e[2] == e1)
      {
        if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
          goto BTOR_REWRITE_EQ_NODE_NO_REWRITE;
        BTOR_INC_REC_RW_CALL (btor);
        if (BTOR_IS_INVERTED_NODE (e0))
        {
          tmp1 =
              btor_rewrite_eq_exp (btor, BTOR_INVERT_NODE (real_e0->e[1]), e1);
          result = btor_rewrite_and_exp (btor, real_e0->e[0], tmp1);
        }
        else
        {
          tmp1   = btor_rewrite_eq_exp (btor, real_e0->e[1], e1);
          result = btor_or_exp (btor, BTOR_INVERT_NODE (real_e0->e[0]), tmp1);
        }
        btor_release_exp (btor, tmp1);
        BTOR_DEC_REC_RW_CALL (btor);
        return result;
      }
    }

    if (BTOR_IS_ARRAY_OR_BV_COND_NODE (real_e1))
    {
      if (real_e1->e[1] == e0)
      {
        if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
          goto BTOR_REWRITE_EQ_NODE_NO_REWRITE;
        BTOR_INC_REC_RW_CALL (btor);
        if (BTOR_IS_INVERTED_NODE (e1))
        {
          tmp1 =
              btor_rewrite_eq_exp (btor, BTOR_INVERT_NODE (real_e1->e[2]), e0);
          result = btor_rewrite_and_exp (
              btor, BTOR_INVERT_NODE (real_e1->e[0]), tmp1);
        }
        else
        {
          tmp1   = btor_rewrite_eq_exp (btor, real_e1->e[2], e0);
          result = btor_or_exp (btor, real_e1->e[0], tmp1);
        }
        btor_release_exp (btor, tmp1);
        BTOR_DEC_REC_RW_CALL (btor);
        return result;
      }

      if (real_e1->e[2] == e0)
      {
        if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
          goto BTOR_REWRITE_EQ_NODE_NO_REWRITE;
        BTOR_INC_REC_RW_CALL (btor);
        if (BTOR_IS_INVERTED_NODE (e1))
        {
          tmp1 =
              btor_rewrite_eq_exp (btor, BTOR_INVERT_NODE (real_e1->e[1]), e0);
          result = btor_rewrite_and_exp (btor, real_e1->e[0], tmp1);
        }
        else
        {
          tmp1   = btor_rewrite_eq_exp (btor, real_e1->e[1], e0);
          result = btor_or_exp (btor, BTOR_INVERT_NODE (real_e1->e[0]), tmp1);
        }
        btor_release_exp (btor, tmp1);
        BTOR_DEC_REC_RW_CALL (btor);
        return result;
      }
    }

    /* normalize adds and muls on demand */
    if (!BTOR_IS_INVERTED_NODE (e0) && !BTOR_IS_INVERTED_NODE (e1)
        && (((e0->kind == BTOR_ADD_NODE || e0->kind == BTOR_MUL_NODE)
             && e0->kind == e1->kind)
            || (e0->kind == BTOR_ADD_NODE && e1->kind == BTOR_MUL_NODE)
            || (e1->kind == BTOR_ADD_NODE && e0->kind == BTOR_MUL_NODE)))
    {
      if ((e0->kind == BTOR_ADD_NODE || e0->kind == BTOR_MUL_NODE)
          && e0->kind == e1->kind)
      {
        if (e0->kind == BTOR_ADD_NODE)
        {
          assert (e1->kind == BTOR_ADD_NODE);
          normalize_adds_exp (btor, e0, e1, &tmp1, &tmp2);
          normalized = 1;
          e0         = tmp1;
          e1         = tmp2;
        }
        else
        {
          assert (e0->kind == BTOR_MUL_NODE);
          assert (e1->kind == BTOR_MUL_NODE);
          normalize_muls_exp (btor, e0, e1, &tmp1, &tmp2);
          normalized = 1;
          e0         = tmp1;
          e1         = tmp2;
        }
      }
      /* find out distributivity from mul and add */
      else if (e0->kind == BTOR_MUL_NODE && e1->kind == BTOR_ADD_NODE)
      {
        tmp1 = try_rewrite_add_mul_distrib (btor, e1->e[0], e1->e[1]);
        if (tmp1)
        {
          if (tmp1 == e0)
          {
            btor_release_exp (btor, tmp1);
            return btor_true_exp (btor);
          }
          btor_release_exp (btor, tmp1);
        }
      }
      else if (e1->kind == BTOR_MUL_NODE && e0->kind == BTOR_ADD_NODE)
      {
        tmp1 = try_rewrite_add_mul_distrib (btor, e0->e[0], e0->e[1]);
        if (tmp1)
        {
          if (tmp1 == e1)
          {
            btor_release_exp (btor, tmp1);
            return btor_true_exp (btor);
          }
          btor_release_exp (btor, tmp1);
        }
      }
    }
    else if (kind == BTOR_BEQ_NODE)
    {
      /* push eq down over concats */
      if ((real_e0->kind == BTOR_CONCAT_NODE
           || real_e1->kind == BTOR_CONCAT_NODE))
      {
        if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
          goto BTOR_REWRITE_EQ_NODE_NO_REWRITE;
        BTOR_INC_REC_RW_CALL (btor);
        upper = real_e0->len - 1;
        if (real_e0->kind == BTOR_CONCAT_NODE)
          lower = upper - BTOR_REAL_ADDR_NODE (real_e0->e[0])->len + 1;
        else
          lower = upper - BTOR_REAL_ADDR_NODE (real_e1->e[0])->len + 1;

        tmp1 = btor_rewrite_slice_exp (btor, e0, upper, lower);
        tmp3 = btor_rewrite_slice_exp (btor, e1, upper, lower);
        tmp2 = btor_rewrite_eq_exp (btor, tmp1, tmp3);
        btor_release_exp (btor, tmp1);
        btor_release_exp (btor, tmp3);
        lower--;
        tmp1 = btor_rewrite_slice_exp (btor, e0, lower, 0);
        tmp3 = btor_rewrite_slice_exp (btor, e1, lower, 0);
        tmp4 = btor_rewrite_eq_exp (btor, tmp1, tmp3);

        result = btor_rewrite_and_exp (btor, tmp2, tmp4);

        btor_release_exp (btor, tmp1);
        btor_release_exp (btor, tmp2);
        btor_release_exp (btor, tmp3);
        btor_release_exp (btor, tmp4);
        BTOR_DEC_REC_RW_CALL (btor);
        return result;
      }
    }
  }

  result = rewrite_binary_exp (btor, kind, e0, e1);
  if (!result)
  {
  BTOR_REWRITE_EQ_NODE_NO_REWRITE:
    result = btor_eq_exp_node (btor, e0, e1);
  }

  if (normalized)
  {
    btor_release_exp (btor, tmp1);
    btor_release_exp (btor, tmp2);
  }

  return result;
}

BtorNode *
btor_rewrite_add_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *result, *e0_norm, *e1_norm, *temp;
  int normalized;

  e0 = btor_pointer_chase_simplified_exp (btor, e0);
  e1 = btor_pointer_chase_simplified_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));
  assert (btor->rewrite_level > 0);

  normalized = 0;

  /* boolean case */
  if (BTOR_REAL_ADDR_NODE (e0)->len == 1)
  {
    if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
      goto BTOR_REWRITE_ADD_NODE_NO_REWRITE;
    BTOR_INC_REC_RW_CALL (btor);
    result = btor_xor_exp (btor, e0, e1);
    BTOR_DEC_REC_RW_CALL (btor);
    return result;
  }

  /* a - a == 0 */
  if (!BTOR_IS_INVERTED_NODE (e1) && e1->kind == BTOR_ADD_NODE
      && e0 == BTOR_INVERT_NODE (e1->e[0]) && is_const_one_exp (btor, e1->e[1]))
  {
    return btor_zero_exp (btor, e1->len);
  }

  if (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e0))
      && !BTOR_IS_INVERTED_NODE (e1) && e1->kind == BTOR_ADD_NODE)
  {
    /* recursion is no problem here, as one call leads to
     * folding of constants, and the other call can not
     * trigger the same kind of recursion anymore */

    if (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e1->e[0])))
    {
      assert (!BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e1->e[1])));
      if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
        goto BTOR_REWRITE_ADD_NODE_NO_REWRITE;
      BTOR_INC_REC_RW_CALL (btor);
      temp   = btor_rewrite_add_exp (btor, e0, e1->e[0]);
      result = btor_rewrite_add_exp (btor, temp, e1->e[1]);
      BTOR_DEC_REC_RW_CALL (btor);
      btor_release_exp (btor, temp);
      return result;
    }

    if (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e1->e[1])))
    {
      assert (!BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e1->e[0])));
      if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
        goto BTOR_REWRITE_ADD_NODE_NO_REWRITE;
      BTOR_INC_REC_RW_CALL (btor);
      temp   = btor_rewrite_add_exp (btor, e0, e1->e[1]);
      result = btor_rewrite_add_exp (btor, temp, e1->e[0]);
      BTOR_DEC_REC_RW_CALL (btor);
      btor_release_exp (btor, temp);
      return result;
    }
  }

  else if (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e1))
           && !BTOR_IS_INVERTED_NODE (e0) && e0->kind == BTOR_ADD_NODE)
  {
    /* recursion is no problem here, as one call leads to
     * folding of constants, and the other call can not
     * trigger the same kind of recursion anymore */

    if (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e0->e[0])))
    {
      assert (!BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e0->e[1])));
      if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
        goto BTOR_REWRITE_ADD_NODE_NO_REWRITE;
      BTOR_INC_REC_RW_CALL (btor);
      temp   = btor_rewrite_add_exp (btor, e1, e0->e[0]);
      result = btor_rewrite_add_exp (btor, temp, e0->e[1]);
      BTOR_DEC_REC_RW_CALL (btor);
      btor_release_exp (btor, temp);
      return result;
    }

    if (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e0->e[1])))
    {
      assert (!BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e0->e[0])));
      if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
        goto BTOR_REWRITE_ADD_NODE_NO_REWRITE;
      BTOR_INC_REC_RW_CALL (btor);
      temp   = btor_rewrite_add_exp (btor, e1, e0->e[1]);
      result = btor_rewrite_add_exp (btor, temp, e0->e[0]);
      BTOR_DEC_REC_RW_CALL (btor);
      btor_release_exp (btor, temp);
      return result;
    }
  }

  if (btor->rewrite_level > 2 && !BTOR_IS_INVERTED_NODE (e0)
      && !BTOR_IS_INVERTED_NODE (e1) && e0->kind == BTOR_MUL_NODE
      && e1->kind == BTOR_MUL_NODE)
  {
    /* normalize muls on demand */
    normalize_muls_exp (btor, e0, e1, &e0_norm, &e1_norm);
    normalized = 1;
    e0         = e0_norm;
    e1         = e1_norm;
  }

  result = rewrite_binary_exp (btor, BTOR_ADD_NODE, e0, e1);
  if (!result)
  {
  BTOR_REWRITE_ADD_NODE_NO_REWRITE:
    result = btor_add_exp_node (btor, e0, e1);
  }

  if (normalized)
  {
    btor_release_exp (btor, e0_norm);
    btor_release_exp (btor, e1_norm);
  }

  return result;
}

BtorNode *
btor_rewrite_mul_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *result, *left, *right;
  int normalized;

  e0 = btor_pointer_chase_simplified_exp (btor, e0);
  e1 = btor_pointer_chase_simplified_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));
  assert (btor->rewrite_level > 0);

  normalized = 0;

  /* we do not need the optimization for term * power_of_2_constant as
   * the AIG level does this optimization already */
  /* boolean case */
  if (BTOR_REAL_ADDR_NODE (e0)->len == 1)
  {
    if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
      goto BTOR_REWRITE_MUL_NODE_NO_REWRITE;
    BTOR_INC_REC_RW_CALL (btor);
    result = btor_rewrite_and_exp (btor, e0, e1);
    BTOR_DEC_REC_RW_CALL (btor);
    return result;
  }

  if (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e0))
      && !BTOR_IS_INVERTED_NODE (e1) && e1->kind == BTOR_MUL_NODE)
  {
    if (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e1->e[0])))
    {
      assert (!BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e1->e[1])));
      if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
        goto BTOR_REWRITE_MUL_NODE_NO_REWRITE;
      BTOR_INC_REC_RW_CALL (btor);
      left   = btor_rewrite_mul_exp (btor, e0, e1->e[0]);
      result = btor_rewrite_mul_exp (btor, left, e1->e[1]);
      BTOR_DEC_REC_RW_CALL (btor);
      btor_release_exp (btor, left);
      return result;
    }

    if (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e1->e[1])))
    {
      assert (!BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e1->e[0])));
      if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
        goto BTOR_REWRITE_MUL_NODE_NO_REWRITE;
      BTOR_INC_REC_RW_CALL (btor);
      left   = btor_rewrite_mul_exp (btor, e0, e1->e[1]);
      result = btor_rewrite_mul_exp (btor, left, e1->e[0]);
      BTOR_DEC_REC_RW_CALL (btor);
      btor_release_exp (btor, left);
      return result;
    }
  }

  else if (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e1))
           && !BTOR_IS_INVERTED_NODE (e0) && e0->kind == BTOR_MUL_NODE)

  {
    if (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e0->e[0])))
    {
      assert (!BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e0->e[1])));
      if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
        goto BTOR_REWRITE_MUL_NODE_NO_REWRITE;
      BTOR_INC_REC_RW_CALL (btor);
      left   = btor_rewrite_mul_exp (btor, e1, e0->e[0]);
      result = btor_rewrite_mul_exp (btor, left, e0->e[1]);
      BTOR_DEC_REC_RW_CALL (btor);
      btor_release_exp (btor, left);
      return result;
    }

    if (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e0->e[1])))
    {
      assert (!BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e0->e[0])));
      if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
        goto BTOR_REWRITE_MUL_NODE_NO_REWRITE;
      BTOR_INC_REC_RW_CALL (btor);
      left   = btor_rewrite_mul_exp (btor, e1, e0->e[1]);
      result = btor_rewrite_mul_exp (btor, left, e0->e[0]);
      BTOR_DEC_REC_RW_CALL (btor);
      btor_release_exp (btor, left);
      return result;
    }
  }

  /* const * (t + const) =recursively= const * t + const * const */
  if (btor->rewrite_level > 2)
  {
    if (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e0))
        && BTOR_IS_REGULAR_NODE (e1) && e1->kind == BTOR_ADD_NODE
        && (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e1->e[0]))
            || BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e1->e[1]))))
    {
      if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
        goto BTOR_REWRITE_MUL_NODE_NO_REWRITE;
      BTOR_INC_REC_RW_CALL (btor);
      left   = btor_rewrite_mul_exp (btor, e0, e1->e[0]);
      right  = btor_rewrite_mul_exp (btor, e0, e1->e[1]);
      result = btor_rewrite_add_exp (btor, left, right);
      BTOR_DEC_REC_RW_CALL (btor);
      btor_release_exp (btor, left);
      btor_release_exp (btor, right);
      return result;
    }

    if (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e1))
        && BTOR_IS_REGULAR_NODE (e0) && e0->kind == BTOR_ADD_NODE
        && (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e0->e[0]))
            || BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e0->e[1]))))
    {
      if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
        goto BTOR_REWRITE_MUL_NODE_NO_REWRITE;
      BTOR_INC_REC_RW_CALL (btor);
      left   = btor_rewrite_mul_exp (btor, e1, e0->e[0]);
      right  = btor_rewrite_mul_exp (btor, e1, e0->e[1]);
      result = btor_rewrite_add_exp (btor, left, right);
      BTOR_DEC_REC_RW_CALL (btor);
      btor_release_exp (btor, left);
      btor_release_exp (btor, right);
      return result;
    }

    if (!BTOR_IS_INVERTED_NODE (e0) && !BTOR_IS_INVERTED_NODE (e1)
        && e0->kind == BTOR_ADD_NODE && e1->kind == BTOR_ADD_NODE)
    {
      /* normalize adds on demand */
      normalize_adds_exp (btor, e0, e1, &left, &right);
      normalized = 1;
      e0         = left;
      e1         = right;
    }
  }

  result = rewrite_binary_exp (btor, BTOR_MUL_NODE, e0, e1);
  if (!result)
  {
  BTOR_REWRITE_MUL_NODE_NO_REWRITE:
    result = btor_mul_exp_node (btor, e0, e1);
  }

  if (normalized)
  {
    btor_release_exp (btor, left);
    btor_release_exp (btor, right);
  }

  return result;
}

BtorNode *
btor_rewrite_ult_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *result, *e0_norm, *e1_norm, *temp;
  int normalized;

  e0 = btor_pointer_chase_simplified_exp (btor, e0);
  e1 = btor_pointer_chase_simplified_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));
  assert (btor->rewrite_level > 0);

  normalized = 0;

  if (BTOR_IS_INVERTED_NODE (e0) && BTOR_IS_INVERTED_NODE (e1))
  {
    /* ~a < ~b  is the same as  b < a */
    temp = BTOR_REAL_ADDR_NODE (e1);
    e1   = BTOR_REAL_ADDR_NODE (e0);
    e0   = temp;
  }

  /* boolean case */
  if (BTOR_REAL_ADDR_NODE (e0)->len == 1)
  {
    if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
      goto BTOR_REWRITE_ULT_NODE_NO_REWRITE;
    BTOR_INC_REC_RW_CALL (btor);
    result = btor_rewrite_and_exp (btor, BTOR_INVERT_NODE (e0), e1);
    BTOR_DEC_REC_RW_CALL (btor);
    return result;
  }

  if (btor->rewrite_level > 2)
  {
    if (!BTOR_IS_INVERTED_NODE (e0) && !BTOR_IS_INVERTED_NODE (e1)
        && e0->kind == e1->kind)
    {
      switch (e0->kind)
      {
        case BTOR_CONCAT_NODE:
          assert (e1->kind == BTOR_CONCAT_NODE);
          if (e0->e[0] == e1->e[0])
          {
            if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
              goto BTOR_REWRITE_ULT_NODE_NO_REWRITE;
            BTOR_INC_REC_RW_CALL (btor);
            result = btor_rewrite_ult_exp (btor, e0->e[1], e1->e[1]);
            BTOR_DEC_REC_RW_CALL (btor);
            return result;
          }
          else if (e0->e[1] == e1->e[1])
          {
            if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
              goto BTOR_REWRITE_ULT_NODE_NO_REWRITE;
            BTOR_INC_REC_RW_CALL (btor);
            result = btor_rewrite_ult_exp (btor, e0->e[0], e1->e[0]);
            BTOR_DEC_REC_RW_CALL (btor);
            return result;
          }
          break;
        case BTOR_ADD_NODE:
          assert (e1->kind == BTOR_ADD_NODE);
          normalize_adds_exp (btor, e0, e1, &e0_norm, &e1_norm);
          normalized = 1;
          e0         = e0_norm;
          e1         = e1_norm;
          break;
        case BTOR_MUL_NODE:
          assert (e1->kind == BTOR_MUL_NODE);
          normalize_muls_exp (btor, e0, e1, &e0_norm, &e1_norm);
          normalized = 1;
          e0         = e0_norm;
          e1         = e1_norm;
          break;
        default: break;
      }
    }
  }

  result = rewrite_binary_exp (btor, BTOR_ULT_NODE, e0, e1);
  if (!result)
  {
  BTOR_REWRITE_ULT_NODE_NO_REWRITE:
    result = btor_ult_exp_node (btor, e0, e1);
  }

  if (normalized)
  {
    btor_release_exp (btor, e0_norm);
    btor_release_exp (btor, e1_norm);
  }

  return result;
}

BtorNode *
btor_rewrite_sll_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *result;

  e0 = btor_pointer_chase_simplified_exp (btor, e0);
  e1 = btor_pointer_chase_simplified_exp (btor, e1);
  assert (btor_precond_shift_exp_dbg (btor, e0, e1));
  assert (btor->rewrite_level > 0);
  result = rewrite_binary_exp (btor, BTOR_SLL_NODE, e0, e1);
  if (!result) result = btor_sll_exp_node (btor, e0, e1);

  return result;
}

BtorNode *
btor_rewrite_srl_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *result;

  e0 = btor_pointer_chase_simplified_exp (btor, e0);
  e1 = btor_pointer_chase_simplified_exp (btor, e1);
  assert (btor_precond_shift_exp_dbg (btor, e0, e1));
  result = rewrite_binary_exp (btor, BTOR_SRL_NODE, e0, e1);
  if (!result) result = btor_srl_exp_node (btor, e0, e1);

  return result;
}

BtorNode *
btor_rewrite_udiv_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *result, *e0_norm, *e1_norm;
  int normalized;

  e0 = btor_pointer_chase_simplified_exp (btor, e0);
  e1 = btor_pointer_chase_simplified_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));
  assert (btor->rewrite_level > 0);

  normalized = 0;

  /* we do not need the optimization for term / power_of_2_constant as
   * the AIG level does this optimization already */

  /* boolean case */
  if (BTOR_REAL_ADDR_NODE (e0)->len == 1)
  {
    if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
      goto BTOR_REWRITE_UDIV_NODE_NO_REWRITE;
    BTOR_INC_REC_RW_CALL (btor);
    result = BTOR_INVERT_NODE (
        btor_rewrite_and_exp (btor, BTOR_INVERT_NODE (e0), e1));
    BTOR_DEC_REC_RW_CALL (btor);
    return result;
  }

  /* normalize adds and muls on demand */
  if (btor->rewrite_level > 2 && !BTOR_IS_INVERTED_NODE (e0)
      && !BTOR_IS_INVERTED_NODE (e1) && e0->kind == e1->kind)
  {
    if (e0->kind == BTOR_ADD_NODE)
    {
      assert (e1->kind == BTOR_ADD_NODE);
      normalize_adds_exp (btor, e0, e1, &e0_norm, &e1_norm);
      normalized = 1;
      e0         = e0_norm;
      e1         = e1_norm;
    }
    else if (e0->kind == BTOR_MUL_NODE)
    {
      assert (e1->kind == BTOR_MUL_NODE);
      normalize_muls_exp (btor, e0, e1, &e0_norm, &e1_norm);
      normalized = 1;
      e0         = e0_norm;
      e1         = e1_norm;
    }
  }

  result = rewrite_binary_exp (btor, BTOR_UDIV_NODE, e0, e1);
  if (!result)
  {
  BTOR_REWRITE_UDIV_NODE_NO_REWRITE:
    result = btor_udiv_exp_node (btor, e0, e1);
  }

  if (normalized)
  {
    btor_release_exp (btor, e0_norm);
    btor_release_exp (btor, e1_norm);
  }

  return result;
}

BtorNode *
btor_rewrite_urem_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *result, *e0_norm, *e1_norm;
  int normalized;

  e0 = btor_pointer_chase_simplified_exp (btor, e0);
  e1 = btor_pointer_chase_simplified_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e0, e1));
  assert (btor->rewrite_level > 0);

  normalized = 0;

  /* we do not need the optimization for term % power_of_2_constant as
   * the AIG level does this optimization already */

  /* boolean case */
  if (BTOR_REAL_ADDR_NODE (e0)->len == 1)
  {
    if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
      goto BTOR_REWRITE_UREM_NODE_NO_REWRITE;
    BTOR_INC_REC_RW_CALL (btor);
    result = btor_rewrite_and_exp (btor, e0, BTOR_INVERT_NODE (e1));
    BTOR_DEC_REC_RW_CALL (btor);
    return result;
  }

  /* normalize adds and muls on demand */
  if (btor->rewrite_level > 2 && !BTOR_IS_INVERTED_NODE (e0)
      && !BTOR_IS_INVERTED_NODE (e1) && e0->kind == e1->kind)
  {
    if (e0->kind == BTOR_ADD_NODE)
    {
      assert (e1->kind == BTOR_ADD_NODE);
      normalize_adds_exp (btor, e0, e1, &e0_norm, &e1_norm);
      normalized = 1;
      e0         = e0_norm;
      e1         = e1_norm;
    }
    else if (e0->kind == BTOR_MUL_NODE)
    {
      assert (e1->kind == BTOR_MUL_NODE);
      normalize_muls_exp (btor, e0, e1, &e0_norm, &e1_norm);
      normalized = 1;
      e0         = e0_norm;
      e1         = e1_norm;
    }
  }

  result = rewrite_binary_exp (btor, BTOR_UREM_NODE, e0, e1);
  if (!result)
  {
  BTOR_REWRITE_UREM_NODE_NO_REWRITE:
    result = btor_urem_exp_node (btor, e0, e1);
  }

  if (normalized)
  {
    btor_release_exp (btor, e0_norm);
    btor_release_exp (btor, e1_norm);
  }

  return result;
}

BtorNode *
btor_rewrite_concat_exp (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *result, *temp, *cur;
  BtorNodePtrStack stack, po_stack;
  BtorMemMgr *mm;
  int i;

  e0 = btor_pointer_chase_simplified_exp (btor, e0);
  e1 = btor_pointer_chase_simplified_exp (btor, e1);
  assert (btor_precond_concat_exp_dbg (btor, e0, e1));
  assert (btor->rewrite_level > 0);

  mm = btor->mm;

  if (BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (e1))
      && BTOR_REAL_ADDR_NODE (e0)->kind == BTOR_CONCAT_NODE
      && BTOR_IS_BV_CONST_NODE (
             BTOR_REAL_ADDR_NODE (BTOR_REAL_ADDR_NODE (e0)->e[1])))
  {
    if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
      goto BTOR_REWRITE_CONCAT_NODE_NO_REWRITE;
    BTOR_INC_REC_RW_CALL (btor);
    if (!BTOR_IS_INVERTED_NODE (e0))
    {
      temp   = btor_rewrite_concat_exp (btor, e0->e[1], e1);
      result = btor_rewrite_concat_exp (btor, e0->e[0], temp);
      btor_release_exp (btor, temp);
    }
    else
    {
      temp = btor_rewrite_concat_exp (
          btor, BTOR_INVERT_NODE (BTOR_REAL_ADDR_NODE (e0)->e[1]), e1);
      result = btor_rewrite_concat_exp (
          btor, BTOR_INVERT_NODE (BTOR_REAL_ADDR_NODE (e0)->e[0]), temp);
      btor_release_exp (btor, temp);
    }
    BTOR_DEC_REC_RW_CALL (btor);
    return result;
  }

  /* normalize concats --> left-associative */
  if (btor->rewrite_level > 2
      && BTOR_REAL_ADDR_NODE (e1)->kind == BTOR_CONCAT_NODE)
  {
    if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
      goto BTOR_REWRITE_CONCAT_NODE_NO_REWRITE;
    BTOR_INC_REC_RW_CALL (btor);

    BTOR_INIT_STACK (po_stack);
    BTOR_PUSH_STACK (mm, po_stack, e0);

    BTOR_INIT_STACK (stack);
    BTOR_PUSH_STACK (mm, stack, e1);
    do
    {
      cur = BTOR_POP_STACK (stack);
      if (BTOR_REAL_ADDR_NODE (cur)->kind == BTOR_CONCAT_NODE)
      {
        BTOR_PUSH_STACK (
            mm,
            stack,
            BTOR_COND_INVERT_NODE (cur, BTOR_REAL_ADDR_NODE (cur)->e[1]));
        BTOR_PUSH_STACK (
            mm,
            stack,
            BTOR_COND_INVERT_NODE (cur, BTOR_REAL_ADDR_NODE (cur)->e[0]));
      }
      else
        BTOR_PUSH_STACK (mm, po_stack, cur);
    } while (!BTOR_EMPTY_STACK (stack));

    assert (BTOR_COUNT_STACK (po_stack) >= 3);
    result =
        btor_rewrite_concat_exp (btor, po_stack.start[0], po_stack.start[1]);
    for (i = 2; i < BTOR_COUNT_STACK (po_stack); i++)
    {
      cur = po_stack.start[i];
      assert (BTOR_REAL_ADDR_NODE (cur)->kind != BTOR_CONCAT_NODE);
      temp = btor_rewrite_concat_exp (btor, result, cur);
      btor_release_exp (btor, result);
      result = temp;
    }

    BTOR_RELEASE_STACK (mm, stack);
    BTOR_RELEASE_STACK (mm, po_stack);

    BTOR_DEC_REC_RW_CALL (btor);
    return result;
  }

  result = rewrite_binary_exp (btor, BTOR_CONCAT_NODE, e0, e1);
  if (!result)
  {
  BTOR_REWRITE_CONCAT_NODE_NO_REWRITE:
    result = btor_concat_exp_node (btor, e0, e1);
  }

  return result;
}

BtorNode *
btor_rewrite_read_exp (Btor *btor, BtorNode *e_array, BtorNode *e_index)
{
  BtorNode *result, *cur_array, *write_index;
  int propagations;

  /* no recurisve rewrite calls here, so we do not need to check bounds */

  e_array = btor_pointer_chase_simplified_exp (btor, e_array);
  e_index = btor_pointer_chase_simplified_exp (btor, e_index);
  assert (btor_precond_read_exp_dbg (btor, e_array, e_index));
  assert (btor->rewrite_level > 0);

  if (BTOR_IS_WRITE_NODE (e_array))
  {
    write_index = e_array->e[1];
    /* if read index is equal write index, then return write value */
    if (e_index == write_index) return btor_copy_exp (btor, e_array->e[2]);

    cur_array = e_array;
    assert (BTOR_IS_REGULAR_NODE (cur_array));
    assert (BTOR_IS_ARRAY_NODE (cur_array));
    propagations = 0;

    do
    {
      assert (BTOR_IS_WRITE_NODE (cur_array));
      write_index = cur_array->e[1];

      if (e_index == write_index) return btor_copy_exp (btor, cur_array->e[2]);

      if (is_always_unequal (btor, e_index, write_index))
      {
        cur_array = cur_array->e[0];
        assert (BTOR_IS_REGULAR_NODE (cur_array));
        assert (BTOR_IS_ARRAY_NODE (cur_array));
        propagations++;
        btor->stats.read_props_construct++;
      }
      else
        break;

    } while (BTOR_IS_WRITE_NODE (cur_array)
             && propagations < BTOR_READ_OVER_WRITE_DOWN_PROPAGATION_LIMIT);

    result = btor_read_exp_node (btor, cur_array, e_index);
  }
  else
    result = btor_read_exp_node (btor, e_array, e_index);

  assert (result);

  return result;
}

BtorNode *
btor_rewrite_write_exp (Btor *btor,
                        BtorNode *e_array,
                        BtorNode *e_index,
                        BtorNode *e_value)
{
  BtorNode *cur, *cur_write, *temp, *result;
  BtorNode *chain[BTOR_WRITE_CHAIN_NODE_RW_BOUND];
  int depth;

  /* no recurisve rewrite calls here, so we do not need to check bounds */

  e_array = btor_pointer_chase_simplified_exp (btor, e_array);
  e_index = btor_pointer_chase_simplified_exp (btor, e_index);
  e_value = btor_pointer_chase_simplified_exp (btor, e_value);
  assert (btor_precond_write_exp_dbg (btor, e_array, e_index, e_value));
  assert (btor->rewrite_level > 0);

  result = 0;

  if (btor->rewrite_level > 2 && BTOR_IS_WRITE_NODE (e_array))
  {
    depth = 0;
    cur   = e_array;
    assert (BTOR_IS_REGULAR_NODE (cur));
    assert (BTOR_IS_WRITE_NODE (cur));

    while (BTOR_IS_WRITE_NODE (cur) && cur->e[1] != e_index
           && depth < BTOR_WRITE_CHAIN_NODE_RW_BOUND)
    {
      assert (BTOR_IS_REGULAR_NODE (cur));
      assert (BTOR_IS_WRITE_NODE (cur));
      chain[depth++] = cur;
      cur            = cur->e[0];
      assert (BTOR_IS_REGULAR_NODE (cur));
      assert (BTOR_IS_ARRAY_NODE (cur));
    }

    if (depth < BTOR_WRITE_CHAIN_NODE_RW_BOUND && BTOR_IS_WRITE_NODE (cur))
    {
      assert (cur->e[1] == e_index);
      /* we overwrite this position anyhow, so we can skip
       * this intermediate write */
      cur = btor_copy_exp (btor, cur->e[0]);
      depth--;
      while (depth >= 0)
      {
        cur_write = chain[depth--];
        assert (BTOR_IS_REGULAR_NODE (cur_write));
        assert (BTOR_IS_WRITE_NODE (cur_write));
        temp =
            btor_write_exp_node (btor, cur, cur_write->e[1], cur_write->e[2]);
        btor_release_exp (btor, cur);
        cur = temp;
      }

      result = btor_write_exp_node (btor, cur, e_index, e_value);
      btor_release_exp (btor, cur);
    }
  }

  if (!result) result = btor_write_exp_node (btor, e_array, e_index, e_value);

  return result;
}

BtorNode *
btor_rewrite_cond_exp (Btor *btor,
                       BtorNode *e_cond,
                       BtorNode *e_if,
                       BtorNode *e_else)
{
  BtorNode *(*fptr) (Btor *, BtorNode *, BtorNode *);
  BtorNode *result, *tmp1, *tmp2, *tmp3, *tmp4;
  BtorNodeKind kind;

  e_cond = btor_pointer_chase_simplified_exp (btor, e_cond);
  e_if   = btor_pointer_chase_simplified_exp (btor, e_if);
  e_else = btor_pointer_chase_simplified_exp (btor, e_else);
  assert (btor_precond_cond_exp_dbg (btor, e_cond, e_if, e_else));
  assert (btor->rewrite_level > 0);

  result = 0;
  kind   = BTOR_BCOND_NODE;

  /* normalization: ~e0 ? e1 : e2 is the same as e0 ? e2: e1 */
  if (BTOR_IS_INVERTED_NODE (e_cond))
  {
    e_cond = BTOR_INVERT_NODE (e_cond);
    tmp1   = e_if;
    e_if   = e_else;
    e_else = tmp1;
  }

  assert (!BTOR_IS_INVERTED_NODE (e_cond));

  if (BTOR_IS_ARRAY_NODE (BTOR_REAL_ADDR_NODE (e_if))) kind = BTOR_ACOND_NODE;

  if (e_if == e_else)
  {
    result = btor_copy_exp (btor, e_if);
    return result;
  }

  if (BTOR_IS_BV_CONST_NODE (e_cond))
  {
    if (e_cond->bits[0] == '1')
      result = btor_copy_exp (btor, e_if);
    else
      result = btor_copy_exp (btor, e_else);
    return result;
  }

  if (BTOR_IS_ARRAY_OR_BV_COND_NODE (BTOR_REAL_ADDR_NODE (e_if)))
  {
    if (BTOR_REAL_ADDR_NODE (e_if)->e[0] == e_cond)
    {
      if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
        goto BTOR_REWRITE_COND_NODE_NO_REWRITE;
      BTOR_INC_REC_RW_CALL (btor);
      result = btor_rewrite_cond_exp (
          btor,
          e_cond,
          BTOR_COND_INVERT_NODE (e_if, BTOR_REAL_ADDR_NODE (e_if)->e[1]),
          e_else);
      BTOR_DEC_REC_RW_CALL (btor);
      return result;
    }

    tmp1 = BTOR_REAL_ADDR_NODE (e_if)->e[0];

    if (BTOR_IS_INVERTED_NODE (e_if))
    {
      tmp2 = BTOR_INVERT_NODE (BTOR_REAL_ADDR_NODE (e_if)->e[1]);
      tmp3 = BTOR_INVERT_NODE (BTOR_REAL_ADDR_NODE (e_if)->e[2]);
    }
    else
    {
      tmp2 = e_if->e[1];
      tmp3 = e_if->e[2];
    }

    if (tmp2 == e_else)
    {
      if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
        goto BTOR_REWRITE_COND_NODE_NO_REWRITE;
      BTOR_INC_REC_RW_CALL (btor);
      tmp4   = btor_rewrite_and_exp (btor, e_cond, BTOR_INVERT_NODE (tmp1));
      result = btor_rewrite_cond_exp (btor, tmp4, tmp3, e_else);
      BTOR_DEC_REC_RW_CALL (btor);
      btor_release_exp (btor, tmp4);
      return result;
    }
    if (tmp3 == e_else)
    {
      if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
        goto BTOR_REWRITE_COND_NODE_NO_REWRITE;
      BTOR_INC_REC_RW_CALL (btor);
      tmp4   = btor_rewrite_and_exp (btor, e_cond, tmp1);
      result = btor_rewrite_cond_exp (btor, tmp4, tmp2, e_else);
      BTOR_DEC_REC_RW_CALL (btor);
      btor_release_exp (btor, tmp4);
      return result;
    }
  }

  if (BTOR_IS_ARRAY_OR_BV_COND_NODE (BTOR_REAL_ADDR_NODE (e_else)))
  {
    if (BTOR_REAL_ADDR_NODE (e_else)->e[0] == e_cond)
    {
      if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
        goto BTOR_REWRITE_COND_NODE_NO_REWRITE;
      BTOR_INC_REC_RW_CALL (btor);
      result = btor_rewrite_cond_exp (
          btor,
          e_cond,
          e_if,
          BTOR_COND_INVERT_NODE (e_else, BTOR_REAL_ADDR_NODE (e_else)->e[2]));
      BTOR_DEC_REC_RW_CALL (btor);
      return result;
    }

    tmp1 = BTOR_REAL_ADDR_NODE (e_else)->e[0];

    if (BTOR_IS_INVERTED_NODE (e_else))
    {
      tmp2 = BTOR_INVERT_NODE (BTOR_REAL_ADDR_NODE (e_else)->e[1]);
      tmp3 = BTOR_INVERT_NODE (BTOR_REAL_ADDR_NODE (e_else)->e[2]);
    }
    else
    {
      tmp2 = e_else->e[1];
      tmp3 = e_else->e[2];
    }

    if (tmp2 == e_if)
    {
      if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
        goto BTOR_REWRITE_COND_NODE_NO_REWRITE;
      BTOR_INC_REC_RW_CALL (btor);
      tmp4 = btor_rewrite_and_exp (
          btor, BTOR_INVERT_NODE (e_cond), BTOR_INVERT_NODE (tmp1));
      result = btor_rewrite_cond_exp (btor, tmp4, tmp3, e_if);
      BTOR_DEC_REC_RW_CALL (btor);
      btor_release_exp (btor, tmp4);
      return result;
    }
    else if (tmp3 == e_if)
    {
      if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
        goto BTOR_REWRITE_COND_NODE_NO_REWRITE;
      BTOR_INC_REC_RW_CALL (btor);
      tmp4   = btor_rewrite_and_exp (btor, BTOR_INVERT_NODE (e_cond), tmp1);
      result = btor_rewrite_cond_exp (btor, tmp4, tmp2, e_if);
      BTOR_DEC_REC_RW_CALL (btor);
      btor_release_exp (btor, tmp4);
      return result;
    }
  }

  if (kind == BTOR_BCOND_NODE)
  {
    if (BTOR_REAL_ADDR_NODE (e_if)->len == 1)
    {
      if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
        goto BTOR_REWRITE_COND_NODE_NO_REWRITE;
      BTOR_INC_REC_RW_CALL (btor);
      tmp1   = btor_or_exp (btor, BTOR_INVERT_NODE (e_cond), e_if);
      tmp2   = btor_or_exp (btor, e_cond, e_else);
      result = btor_rewrite_and_exp (btor, tmp1, tmp2);
      BTOR_DEC_REC_RW_CALL (btor);
      btor_release_exp (btor, tmp1);
      btor_release_exp (btor, tmp2);
    }
    else if (!BTOR_IS_INVERTED_NODE (e_if) && e_if->kind == BTOR_ADD_NODE
             && ((e_if->e[0] == e_else && is_const_one_exp (btor, e_if->e[1]))
                 || (e_if->e[1] == e_else
                     && is_const_one_exp (btor, e_if->e[0]))))
    {
      if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
        goto BTOR_REWRITE_COND_NODE_NO_REWRITE;
      BTOR_INC_REC_RW_CALL (btor);
      tmp1 = btor_uext_exp (btor, e_cond, BTOR_REAL_ADDR_NODE (e_if)->len - 1);
      result = btor_rewrite_add_exp (btor, e_else, tmp1);
      BTOR_DEC_REC_RW_CALL (btor);
      btor_release_exp (btor, tmp1);
    }
    else if (!BTOR_IS_INVERTED_NODE (e_else) && e_else->kind == BTOR_ADD_NODE
             && ((e_else->e[0] == e_if && is_const_one_exp (btor, e_else->e[1]))
                 || (e_else->e[1] == e_if
                     && is_const_one_exp (btor, e_else->e[0]))))
    {
      if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
        goto BTOR_REWRITE_COND_NODE_NO_REWRITE;
      BTOR_INC_REC_RW_CALL (btor);
      tmp1 = btor_uext_exp (
          btor, BTOR_INVERT_NODE (e_cond), BTOR_REAL_ADDR_NODE (e_if)->len - 1);
      result = btor_rewrite_add_exp (btor, e_if, tmp1);
      BTOR_DEC_REC_RW_CALL (btor);
      btor_release_exp (btor, tmp1);
    }
    else if (btor->rewrite_level > 2 && !BTOR_IS_INVERTED_NODE (e_if)
             && !BTOR_IS_INVERTED_NODE (e_else) && e_if->kind == e_else->kind)
    {
      fptr = 0;
      switch (e_if->kind)
      {
        case BTOR_ADD_NODE: fptr = btor_rewrite_add_exp; break;
        case BTOR_AND_NODE: fptr = btor_rewrite_and_exp; break;
        case BTOR_MUL_NODE: fptr = btor_rewrite_mul_exp; break;
        case BTOR_UDIV_NODE: fptr = btor_rewrite_udiv_exp; break;
        case BTOR_UREM_NODE: fptr = btor_rewrite_urem_exp; break;
        default: break;
      }

      if (fptr)
      {
        if (e_if->e[0] == e_else->e[0])
        {
          if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
            goto BTOR_REWRITE_COND_NODE_NO_REWRITE;
          BTOR_INC_REC_RW_CALL (btor);
          tmp1 = btor_rewrite_cond_exp (btor, e_cond, e_if->e[1], e_else->e[1]);
          result = fptr (btor, e_if->e[0], tmp1);
          BTOR_DEC_REC_RW_CALL (btor);
          btor_release_exp (btor, tmp1);
        }
        else if (e_if->e[1] == e_else->e[1])
        {
          if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
            goto BTOR_REWRITE_COND_NODE_NO_REWRITE;
          BTOR_INC_REC_RW_CALL (btor);
          tmp1 = btor_rewrite_cond_exp (btor, e_cond, e_if->e[0], e_else->e[0]);
          result = fptr (btor, tmp1, e_if->e[1]);
          BTOR_DEC_REC_RW_CALL (btor);
          btor_release_exp (btor, tmp1);
        }
        else if (fptr != btor_rewrite_udiv_exp && fptr != btor_rewrite_urem_exp)
        {
          /* works only for commutative operators: */
          if (e_if->e[0] == e_else->e[1])
          {
            if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
              goto BTOR_REWRITE_COND_NODE_NO_REWRITE;
            BTOR_INC_REC_RW_CALL (btor);
            tmp1 =
                btor_rewrite_cond_exp (btor, e_cond, e_if->e[1], e_else->e[0]);
            result = fptr (btor, e_if->e[0], tmp1);
            BTOR_DEC_REC_RW_CALL (btor);
            btor_release_exp (btor, tmp1);
          }
          else if (e_if->e[1] == e_else->e[0])
          {
            if (btor->rec_rw_calls >= BTOR_REC_RW_BOUND)
              goto BTOR_REWRITE_COND_NODE_NO_REWRITE;
            BTOR_INC_REC_RW_CALL (btor);
            tmp1 =
                btor_rewrite_cond_exp (btor, e_cond, e_if->e[0], e_else->e[1]);
            result = fptr (btor, e_if->e[1], tmp1);
            BTOR_DEC_REC_RW_CALL (btor);
            btor_release_exp (btor, tmp1);
          }
        }
      }
    }
  }

  if (!result)
  {
  BTOR_REWRITE_COND_NODE_NO_REWRITE:
    result = btor_cond_exp_node (btor, e_cond, e_if, e_else);
  }

  return result;
}
