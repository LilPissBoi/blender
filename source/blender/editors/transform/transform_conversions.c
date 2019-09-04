/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 */

/** \file
 * \ingroup edtransform
 */

#include "DNA_anim_types.h"
#include "DNA_armature_types.h"
#include "DNA_brush_types.h"
#include "DNA_space_types.h"
#include "DNA_constraint_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_mask_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_math.h"
#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_kdtree.h"

#include "BKE_animsys.h"
#include "BKE_armature.h"
#include "BKE_context.h"
#include "BKE_fcurve.h"
#include "BKE_global.h"
#include "BKE_gpencil.h"
#include "BKE_layer.h"
#include "BKE_key.h"
#include "BKE_main.h"
#include "BKE_modifier.h"
#include "BKE_movieclip.h"
#include "BKE_nla.h"
#include "BKE_node.h"
#include "BKE_object.h"
#include "BKE_particle.h"
#include "BKE_paint.h"
#include "BKE_pointcache.h"
#include "BKE_rigidbody.h"
#include "BKE_scene.h"
#include "BKE_sequencer.h"
#include "BKE_editmesh.h"
#include "BKE_tracking.h"
#include "BKE_mask.h"
#include "BKE_colortools.h"

#include "BIK_api.h"

#include "ED_anim_api.h"
#include "ED_armature.h"
#include "ED_particle.h"
#include "ED_image.h"
#include "ED_keyframing.h"
#include "ED_keyframes_edit.h"
#include "ED_object.h"
#include "ED_markers.h"
#include "ED_mesh.h"
#include "ED_node.h"
#include "ED_clip.h"
#include "ED_mask.h"
#include "ED_gpencil.h"

#include "WM_api.h" /* for WM_event_add_notifier to deal with stabilization nodes */
#include "WM_types.h"

#include "UI_interface.h"

#include "RNA_access.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_build.h"

#include "transform.h"
#include "transform_conversions.h"

/**
 * Transforming around ourselves is no use, fallback to individual origins,
 * useful for curve/armatures.
 */
void transform_around_single_fallback(TransInfo *t)
{
  if ((t->data_len_all == 1) &&
      (ELEM(t->around, V3D_AROUND_CENTER_BOUNDS, V3D_AROUND_CENTER_MEDIAN, V3D_AROUND_ACTIVE)) &&
      (ELEM(t->mode, TFM_RESIZE, TFM_ROTATION, TFM_TRACKBALL))) {
    t->around = V3D_AROUND_LOCAL_ORIGINS;
  }
}

/* ************************** Functions *************************** */

static int trans_data_compare_dist(const void *a, const void *b)
{
  const TransData *td_a = (const TransData *)a;
  const TransData *td_b = (const TransData *)b;

  if (td_a->dist < td_b->dist) {
    return -1;
  }
  else if (td_a->dist > td_b->dist) {
    return 1;
  }
  else {
    return 0;
  }
}

static int trans_data_compare_rdist(const void *a, const void *b)
{
  const TransData *td_a = (const TransData *)a;
  const TransData *td_b = (const TransData *)b;

  if (td_a->rdist < td_b->rdist) {
    return -1;
  }
  else if (td_a->rdist > td_b->rdist) {
    return 1;
  }
  else {
    return 0;
  }
}

static void sort_trans_data_dist_container(const TransInfo *t, TransDataContainer *tc)
{
  TransData *start = tc->data;
  int i;

  for (i = 0; i < tc->data_len && start->flag & TD_SELECTED; i++) {
    start++;
  }

  if (i < tc->data_len) {
    if (t->flag & T_PROP_CONNECTED) {
      qsort(start, tc->data_len - i, sizeof(TransData), trans_data_compare_dist);
    }
    else {
      qsort(start, tc->data_len - i, sizeof(TransData), trans_data_compare_rdist);
    }
  }
}
void sort_trans_data_dist(TransInfo *t)
{
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    sort_trans_data_dist_container(t, tc);
  }
}

/**
 * Make #TD_SELECTED first in the array.
 */
static void sort_trans_data_selected_first_container(TransDataContainer *tc)
{
  TransData *sel, *unsel;
  TransData temp;
  unsel = tc->data;
  sel = tc->data;
  sel += tc->data_len - 1;
  while (sel > unsel) {
    while (unsel->flag & TD_SELECTED) {
      unsel++;
      if (unsel == sel) {
        return;
      }
    }
    while (!(sel->flag & TD_SELECTED)) {
      sel--;
      if (unsel == sel) {
        return;
      }
    }
    temp = *unsel;
    *unsel = *sel;
    *sel = temp;
    sel--;
    unsel++;
  }
}
static void sort_trans_data_selected_first(TransInfo *t)
{
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    sort_trans_data_selected_first_container(tc);
  }
}

/**
 * Distance calculated from not-selected vertex to nearest selected vertex.
 */
static void set_prop_dist(TransInfo *t, const bool with_dist)
{
  int a;

  float _proj_vec[3];
  const float *proj_vec = NULL;

  /* support for face-islands */
  const bool use_island = transdata_check_local_islands(t, t->around);

  if (t->flag & T_PROP_PROJECTED) {
    if (t->spacetype == SPACE_VIEW3D && t->ar && t->ar->regiontype == RGN_TYPE_WINDOW) {
      RegionView3D *rv3d = t->ar->regiondata;
      normalize_v3_v3(_proj_vec, rv3d->viewinv[2]);
      proj_vec = _proj_vec;
    }
  }

  /* Count number of selected. */
  int td_table_len = 0;
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    TransData *td = tc->data;
    for (a = 0; a < tc->data_len; a++, td++) {
      if (td->flag & TD_SELECTED) {
        td_table_len++;
      }
      else {
        /* By definition transform-data has selected items in beginning. */
        break;
      }
    }
  }

  /* Pointers to selected's #TransData.
   * Used to find #TransData from the index returned by #BLI_kdtree_find_nearest. */
  TransData **td_table = MEM_mallocN(sizeof(*td_table) * td_table_len, __func__);

  /* Create and fill kd-tree of selected's positions - in global or proj_vec space. */
  KDTree_3d *td_tree = BLI_kdtree_3d_new(td_table_len);

  int td_table_index = 0;
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    TransData *td = tc->data;
    for (a = 0; a < tc->data_len; a++, td++) {
      if (td->flag & TD_SELECTED) {
        /* Initialize, it was mallocced. */
        float vec[3];
        td->rdist = 0.0f;

        if (use_island) {
          if (tc->use_local_mat) {
            mul_v3_m4v3(vec, tc->mat, td->iloc);
          }
          else {
            mul_v3_m3v3(vec, td->mtx, td->iloc);
          }
        }
        else {
          if (tc->use_local_mat) {
            mul_v3_m4v3(vec, tc->mat, td->center);
          }
          else {
            mul_v3_m3v3(vec, td->mtx, td->center);
          }
        }

        if (proj_vec) {
          float vec_p[3];
          project_v3_v3v3(vec_p, vec, proj_vec);
          sub_v3_v3(vec, vec_p);
        }

        BLI_kdtree_3d_insert(td_tree, td_table_index, vec);
        td_table[td_table_index++] = td;
      }
      else {
        /* By definition transform-data has selected items in beginning. */
        break;
      }
    }
  }
  BLI_assert(td_table_index == td_table_len);

  BLI_kdtree_3d_balance(td_tree);

  /* For each non-selected vertex, find distance to the nearest selected vertex. */
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    TransData *td = tc->data;
    for (a = 0; a < tc->data_len; a++, td++) {
      if ((td->flag & TD_SELECTED) == 0) {
        float vec[3];

        if (use_island) {
          if (tc->use_local_mat) {
            mul_v3_m4v3(vec, tc->mat, td->iloc);
          }
          else {
            mul_v3_m3v3(vec, td->mtx, td->iloc);
          }
        }
        else {
          if (tc->use_local_mat) {
            mul_v3_m4v3(vec, tc->mat, td->center);
          }
          else {
            mul_v3_m3v3(vec, td->mtx, td->center);
          }
        }

        if (proj_vec) {
          float vec_p[3];
          project_v3_v3v3(vec_p, vec, proj_vec);
          sub_v3_v3(vec, vec_p);
        }

        KDTreeNearest_3d nearest;
        const int td_index = BLI_kdtree_3d_find_nearest(td_tree, vec, &nearest);

        td->rdist = -1.0f;
        if (td_index != -1) {
          td->rdist = nearest.dist;
          if (use_island) {
            copy_v3_v3(td->center, td_table[td_index]->center);
            copy_m3_m3(td->axismtx, td_table[td_index]->axismtx);
          }
        }

        if (with_dist) {
          td->dist = td->rdist;
        }
      }
    }
  }

  BLI_kdtree_3d_free(td_tree);
  MEM_freeN(td_table);
}

/* ********************* pose mode ************* */

static short apply_targetless_ik(Object *ob)
{
  bPoseChannel *pchan, *parchan, *chanlist[256];
  bKinematicConstraint *data;
  int segcount, apply = 0;

  /* now we got a difficult situation... we have to find the
   * target-less IK pchans, and apply transformation to the all
   * pchans that were in the chain */

  for (pchan = ob->pose->chanbase.first; pchan; pchan = pchan->next) {
    data = has_targetless_ik(pchan);
    if (data && (data->flag & CONSTRAINT_IK_AUTO)) {

      /* fill the array with the bones of the chain (armature.c does same, keep it synced) */
      segcount = 0;

      /* exclude tip from chain? */
      if (!(data->flag & CONSTRAINT_IK_TIP)) {
        parchan = pchan->parent;
      }
      else {
        parchan = pchan;
      }

      /* Find the chain's root & count the segments needed */
      for (; parchan; parchan = parchan->parent) {
        chanlist[segcount] = parchan;
        segcount++;

        if (segcount == data->rootbone || segcount > 255) {
          break;  // 255 is weak
        }
      }
      for (; segcount; segcount--) {
        Bone *bone;
        float rmat[4][4] /*, tmat[4][4], imat[4][4]*/;

        /* pose_mat(b) = pose_mat(b-1) * offs_bone * channel * constraint * IK  */
        /* we put in channel the entire result of rmat = (channel * constraint * IK) */
        /* pose_mat(b) = pose_mat(b-1) * offs_bone * rmat  */
        /* rmat = pose_mat(b) * inv(pose_mat(b-1) * offs_bone ) */

        parchan = chanlist[segcount - 1];
        bone = parchan->bone;
        bone->flag |= BONE_TRANSFORM; /* ensures it gets an auto key inserted */

        BKE_armature_mat_pose_to_bone(parchan, parchan->pose_mat, rmat);

        /* apply and decompose, doesn't work for constraints or non-uniform scale well */
        {
          float rmat3[3][3], qrmat[3][3], imat3[3][3], smat[3][3];
          copy_m3_m4(rmat3, rmat);

          /* rotation */
          /* [#22409] is partially caused by this, as slight numeric error introduced during
           * the solving process leads to locked-axis values changing. However, we cannot modify
           * the values here, or else there are huge discrepancies between IK-solver (interactive)
           * and applied poses. */
          BKE_pchan_mat3_to_rot(parchan, rmat3, false);

          /* for size, remove rotation */
          /* causes problems with some constraints (so apply only if needed) */
          if (data->flag & CONSTRAINT_IK_STRETCH) {
            BKE_pchan_rot_to_mat3(parchan, qrmat);
            invert_m3_m3(imat3, qrmat);
            mul_m3_m3m3(smat, rmat3, imat3);
            mat3_to_size(parchan->size, smat);
          }

          /* causes problems with some constraints (e.g. childof), so disable this */
          /* as it is IK shouldn't affect location directly */
          /* copy_v3_v3(parchan->loc, rmat[3]); */
        }
      }

      apply = 1;
      data->flag &= ~CONSTRAINT_IK_AUTO;
    }
  }

  return apply;
}

static void bone_children_clear_transflag(int mode, short around, ListBase *lb)
{
  Bone *bone = lb->first;

  for (; bone; bone = bone->next) {
    if ((bone->flag & BONE_HINGE) && (bone->flag & BONE_CONNECTED)) {
      bone->flag |= BONE_HINGE_CHILD_TRANSFORM;
    }
    else if ((bone->flag & BONE_TRANSFORM) && (mode == TFM_ROTATION || mode == TFM_TRACKBALL) &&
             (around == V3D_AROUND_LOCAL_ORIGINS)) {
      bone->flag |= BONE_TRANSFORM_CHILD;
    }
    else {
      bone->flag &= ~(BONE_TRANSFORM | BONE_TRANSFORM_MIRROR);
    }

    bone_children_clear_transflag(mode, around, &bone->childbase);
  }
}

/* sets transform flags in the bones
 * returns total number of bones with BONE_TRANSFORM */
int count_set_pose_transflags(Object *ob,
                              const int mode,
                              const short around,
                              bool has_translate_rotate[2])
{
  bArmature *arm = ob->data;
  bPoseChannel *pchan;
  Bone *bone;
  int total = 0;

  for (pchan = ob->pose->chanbase.first; pchan; pchan = pchan->next) {
    bone = pchan->bone;
    if (PBONE_VISIBLE(arm, bone)) {
      if ((bone->flag & BONE_SELECTED)) {
        bone->flag |= BONE_TRANSFORM;
      }
      else {
        bone->flag &= ~(BONE_TRANSFORM | BONE_TRANSFORM_MIRROR);
      }

      bone->flag &= ~BONE_HINGE_CHILD_TRANSFORM;
      bone->flag &= ~BONE_TRANSFORM_CHILD;
    }
    else {
      bone->flag &= ~(BONE_TRANSFORM | BONE_TRANSFORM_MIRROR);
    }
  }

  /* make sure no bone can be transformed when a parent is transformed */
  /* since pchans are depsgraph sorted, the parents are in beginning of list */
  if (!ELEM(mode, TFM_BONESIZE, TFM_BONE_ENVELOPE_DIST)) {
    for (pchan = ob->pose->chanbase.first; pchan; pchan = pchan->next) {
      bone = pchan->bone;
      if (bone->flag & BONE_TRANSFORM) {
        bone_children_clear_transflag(mode, around, &bone->childbase);
      }
    }
  }
  /* now count, and check if we have autoIK or have to switch from translate to rotate */
  for (pchan = ob->pose->chanbase.first; pchan; pchan = pchan->next) {
    bone = pchan->bone;
    if (bone->flag & BONE_TRANSFORM) {
      total++;

      if (has_translate_rotate != NULL) {
        if (has_targetless_ik(pchan) == NULL) {
          if (pchan->parent && (pchan->bone->flag & BONE_CONNECTED)) {
            if (pchan->bone->flag & BONE_HINGE_CHILD_TRANSFORM) {
              has_translate_rotate[0] = true;
            }
          }
          else {
            if ((pchan->protectflag & OB_LOCK_LOC) != OB_LOCK_LOC) {
              has_translate_rotate[0] = true;
            }
          }
          if ((pchan->protectflag & OB_LOCK_ROT) != OB_LOCK_ROT) {
            has_translate_rotate[1] = true;
          }
        }
        else {
          has_translate_rotate[0] = true;
        }
      }
    }
  }

  return total;
}

/* -------- Auto-IK ---------- */

/* adjust pose-channel's auto-ik chainlen */
static bool pchan_autoik_adjust(bPoseChannel *pchan, short chainlen)
{
  bConstraint *con;
  bool changed = false;

  /* don't bother to search if no valid constraints */
  if ((pchan->constflag & (PCHAN_HAS_IK | PCHAN_HAS_TARGET)) == 0) {
    return changed;
  }

  /* check if pchan has ik-constraint */
  for (con = pchan->constraints.first; con; con = con->next) {
    if (con->type == CONSTRAINT_TYPE_KINEMATIC && (con->enforce != 0.0f)) {
      bKinematicConstraint *data = con->data;

      /* only accept if a temporary one (for auto-ik) */
      if (data->flag & CONSTRAINT_IK_TEMP) {
        /* chainlen is new chainlen, but is limited by maximum chainlen */
        const int old_rootbone = data->rootbone;
        if ((chainlen == 0) || (chainlen > data->max_rootbone)) {
          data->rootbone = data->max_rootbone;
        }
        else {
          data->rootbone = chainlen;
        }
        changed |= (data->rootbone != old_rootbone);
      }
    }
  }

  return changed;
}

/* change the chain-length of auto-ik */
void transform_autoik_update(TransInfo *t, short mode)
{
  Main *bmain = CTX_data_main(t->context);

  short *chainlen = &t->settings->autoik_chainlen;
  bPoseChannel *pchan;

  /* mode determines what change to apply to chainlen */
  if (mode == 1) {
    /* mode=1 is from WHEELMOUSEDOWN... increases len */
    (*chainlen)++;
  }
  else if (mode == -1) {
    /* mode==-1 is from WHEELMOUSEUP... decreases len */
    if (*chainlen > 0) {
      (*chainlen)--;
    }
    else {
      /* IK length did not change, skip updates. */
      return;
    }
  }

  /* apply to all pose-channels */
  bool changed = false;

  FOREACH_TRANS_DATA_CONTAINER (t, tc) {

    /* sanity checks (don't assume t->poseobj is set, or that it is an armature) */
    if (ELEM(NULL, tc->poseobj, tc->poseobj->pose)) {
      continue;
    }

    for (pchan = tc->poseobj->pose->chanbase.first; pchan; pchan = pchan->next) {
      changed |= pchan_autoik_adjust(pchan, *chainlen);
    }
  }

  if (changed) {
    /* TODO(sergey): Consider doing partial update only. */
    DEG_relations_tag_update(bmain);
  }
}

/* frees temporal IKs */
static void pose_grab_with_ik_clear(Main *bmain, Object *ob)
{
  bKinematicConstraint *data;
  bPoseChannel *pchan;
  bConstraint *con, *next;
  bool relations_changed = false;

  for (pchan = ob->pose->chanbase.first; pchan; pchan = pchan->next) {
    /* clear all temporary lock flags */
    pchan->ikflag &= ~(BONE_IK_NO_XDOF_TEMP | BONE_IK_NO_YDOF_TEMP | BONE_IK_NO_ZDOF_TEMP);

    pchan->constflag &= ~(PCHAN_HAS_IK | PCHAN_HAS_TARGET);

    /* remove all temporary IK-constraints added */
    for (con = pchan->constraints.first; con; con = next) {
      next = con->next;
      if (con->type == CONSTRAINT_TYPE_KINEMATIC) {
        data = con->data;
        if (data->flag & CONSTRAINT_IK_TEMP) {
          relations_changed = true;

          /* iTaSC needs clear for removed constraints */
          BIK_clear_data(ob->pose);

          BLI_remlink(&pchan->constraints, con);
          MEM_freeN(con->data);
          MEM_freeN(con);
          continue;
        }
        pchan->constflag |= PCHAN_HAS_IK;
        if (data->tar == NULL || (data->tar->type == OB_ARMATURE && data->subtarget[0] == 0)) {
          pchan->constflag |= PCHAN_HAS_TARGET;
        }
      }
    }
  }

  if (relations_changed) {
    /* TODO(sergey): Consider doing partial update only. */
    DEG_relations_tag_update(bmain);
  }
}

/* ********************* curve/surface ********* */

void calc_distanceCurveVerts(TransData *head, TransData *tail)
{
  TransData *td, *td_near = NULL;
  for (td = head; td <= tail; td++) {
    if (td->flag & TD_SELECTED) {
      td_near = td;
      td->dist = 0.0f;
    }
    else if (td_near) {
      float dist;
      float vec[3];

      sub_v3_v3v3(vec, td_near->center, td->center);
      mul_m3_v3(head->mtx, vec);
      dist = len_v3(vec);

      if (dist < (td - 1)->dist) {
        td->dist = (td - 1)->dist;
      }
      else {
        td->dist = dist;
      }
    }
    else {
      td->dist = FLT_MAX;
      td->flag |= TD_NOTCONNECTED;
    }
  }
  td_near = NULL;
  for (td = tail; td >= head; td--) {
    if (td->flag & TD_SELECTED) {
      td_near = td;
      td->dist = 0.0f;
    }
    else if (td_near) {
      float dist;
      float vec[3];

      sub_v3_v3v3(vec, td_near->center, td->center);
      mul_m3_v3(head->mtx, vec);
      dist = len_v3(vec);

      if (td->flag & TD_NOTCONNECTED || dist < td->dist || (td + 1)->dist < td->dist) {
        td->flag &= ~TD_NOTCONNECTED;
        if (dist < (td + 1)->dist) {
          td->dist = (td + 1)->dist;
        }
        else {
          td->dist = dist;
        }
      }
    }
  }
}

/* Utility function for getting the handle data from bezier's */
TransDataCurveHandleFlags *initTransDataCurveHandles(TransData *td, struct BezTriple *bezt)
{
  TransDataCurveHandleFlags *hdata;
  td->flag |= TD_BEZTRIPLE;
  hdata = td->hdata = MEM_mallocN(sizeof(TransDataCurveHandleFlags), "CuHandle Data");
  hdata->ih1 = bezt->h1;
  hdata->h1 = &bezt->h1;
  hdata->ih2 = bezt->h2; /* in case the second is not selected */
  hdata->h2 = &bezt->h2;
  return hdata;
}

/* ******************* particle edit **************** */

void flushTransParticles(TransInfo *t)
{
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    Scene *scene = t->scene;
    ViewLayer *view_layer = t->view_layer;
    Object *ob = OBACT(view_layer);
    PTCacheEdit *edit = PE_get_current(scene, ob);
    ParticleSystem *psys = edit->psys;
    PTCacheEditPoint *point;
    PTCacheEditKey *key;
    TransData *td;
    float mat[4][4], imat[4][4], co[3];
    int i, k;
    const bool is_prop_edit = (t->flag & T_PROP_EDIT) != 0;

    /* we do transform in world space, so flush world space position
     * back to particle local space (only for hair particles) */
    td = tc->data;
    for (i = 0, point = edit->points; i < edit->totpoint; i++, point++, td++) {
      if (!(point->flag & PEP_TRANSFORM)) {
        continue;
      }

      if (psys && !(psys->flag & PSYS_GLOBAL_HAIR)) {
        ParticleSystemModifierData *psmd_eval = edit->psmd_eval;
        psys_mat_hair_to_global(
            ob, psmd_eval->mesh_final, psys->part->from, psys->particles + i, mat);
        invert_m4_m4(imat, mat);

        for (k = 0, key = point->keys; k < point->totkey; k++, key++) {
          copy_v3_v3(co, key->world_co);
          mul_m4_v3(imat, co);

          /* optimization for proportional edit */
          if (!is_prop_edit || !compare_v3v3(key->co, co, 0.0001f)) {
            copy_v3_v3(key->co, co);
            point->flag |= PEP_EDIT_RECALC;
          }
        }
      }
      else {
        point->flag |= PEP_EDIT_RECALC;
      }
    }

    PE_update_object(t->depsgraph, scene, OBACT(view_layer), 1);
    BKE_particle_batch_cache_dirty_tag(psys, BKE_PARTICLE_BATCH_DIRTY_ALL);
    DEG_id_tag_update(&ob->id, ID_RECALC_PSYS_REDO);
  }
}

/* *** NODE EDITOR *** */
void flushTransNodes(TransInfo *t)
{
  const float dpi_fac = UI_DPI_FAC;

  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    int a;
    TransData *td;
    TransData2D *td2d;

    applyGridAbsolute(t);

    /* flush to 2d vector from internally used 3d vector */
    for (a = 0, td = tc->data, td2d = tc->data_2d; a < tc->data_len; a++, td++, td2d++) {
      bNode *node = td->extra;
      float locx, locy;

      /* weirdo - but the node system is a mix of free 2d elements and dpi sensitive UI */
#ifdef USE_NODE_CENTER
      locx = (td2d->loc[0] - (BLI_rctf_size_x(&node->totr)) * +0.5f) / dpi_fac;
      locy = (td2d->loc[1] - (BLI_rctf_size_y(&node->totr)) * -0.5f) / dpi_fac;
#else
      locx = td2d->loc[0] / dpi_fac;
      locy = td2d->loc[1] / dpi_fac;
#endif

      /* account for parents (nested nodes) */
      if (node->parent) {
        nodeFromView(node->parent, locx, locy, &node->locx, &node->locy);
      }
      else {
        node->locx = locx;
        node->locy = locy;
      }
    }

    /* handle intersection with noodles */
    if (tc->data_len == 1) {
      ED_node_link_intersect_test(t->sa, 1);
    }
  }
}

/* *** SEQUENCE EDITOR *** */

/* commented _only_ because the meta may have animation data which
 * needs moving too [#28158] */

#define SEQ_TX_NESTED_METAS

BLI_INLINE void trans_update_seq(Scene *sce, Sequence *seq, int old_start, int sel_flag)
{
  if (seq->depth == 0) {
    /* Calculate this strip and all nested strips.
     * Children are ALWAYS transformed first so we don't need to do this in another loop.
     */
    BKE_sequence_calc(sce, seq);
  }
  else {
    BKE_sequence_calc_disp(sce, seq);
  }

  if (sel_flag == SELECT) {
    BKE_sequencer_offset_animdata(sce, seq, seq->start - old_start);
  }
}

void flushTransSeq(TransInfo *t)
{
  /* Editing null check already done */
  ListBase *seqbasep = BKE_sequencer_editing_get(t->scene, false)->seqbasep;

  int a, new_frame;
  TransData *td = NULL;
  TransData2D *td2d = NULL;
  TransDataSeq *tdsq = NULL;
  Sequence *seq;

  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);

  /* prevent updating the same seq twice
   * if the transdata order is changed this will mess up
   * but so will TransDataSeq */
  Sequence *seq_prev = NULL;
  int old_start_prev = 0, sel_flag_prev = 0;

  /* flush to 2d vector from internally used 3d vector */
  for (a = 0, td = tc->data, td2d = tc->data_2d; a < tc->data_len; a++, td++, td2d++) {
    int old_start;
    tdsq = (TransDataSeq *)td->extra;
    seq = tdsq->seq;
    old_start = seq->start;
    new_frame = round_fl_to_int(td2d->loc[0]);

    switch (tdsq->sel_flag) {
      case SELECT:
#ifdef SEQ_TX_NESTED_METAS
        if ((seq->depth != 0 || BKE_sequence_tx_test(seq))) {
          /* for meta's, their children move */
          seq->start = new_frame - tdsq->start_offset;
        }
#else
        if (seq->type != SEQ_TYPE_META && (seq->depth != 0 || seq_tx_test(seq))) {
          /* for meta's, their children move */
          seq->start = new_frame - tdsq->start_offset;
        }
#endif
        if (seq->depth == 0) {
          seq->machine = round_fl_to_int(td2d->loc[1]);
          CLAMP(seq->machine, 1, MAXSEQ);
        }
        break;
      case SEQ_LEFTSEL: /* no vertical transform  */
        BKE_sequence_tx_set_final_left(seq, new_frame);
        BKE_sequence_tx_handle_xlimits(seq, tdsq->flag & SEQ_LEFTSEL, tdsq->flag & SEQ_RIGHTSEL);

        /* todo - move this into aftertrans update? - old seq tx needed it anyway */
        BKE_sequence_single_fix(seq);
        break;
      case SEQ_RIGHTSEL: /* no vertical transform  */
        BKE_sequence_tx_set_final_right(seq, new_frame);
        BKE_sequence_tx_handle_xlimits(seq, tdsq->flag & SEQ_LEFTSEL, tdsq->flag & SEQ_RIGHTSEL);

        /* todo - move this into aftertrans update? - old seq tx needed it anyway */
        BKE_sequence_single_fix(seq);
        break;
    }

    /* Update *previous* seq! Else, we would update a seq after its first transform,
     * and if it has more than one (like e.g. SEQ_LEFTSEL and SEQ_RIGHTSEL),
     * the others are not updated! See T38469.
     */
    if (seq != seq_prev) {
      if (seq_prev) {
        trans_update_seq(t->scene, seq_prev, old_start_prev, sel_flag_prev);
      }

      seq_prev = seq;
      old_start_prev = old_start;
      sel_flag_prev = tdsq->sel_flag;
    }
    else {
      /* We want to accumulate *all* sel_flags for this seq! */
      sel_flag_prev |= tdsq->sel_flag;
    }
  }

  /* Don't forget to update the last seq! */
  if (seq_prev) {
    trans_update_seq(t->scene, seq_prev, old_start_prev, sel_flag_prev);
  }

  /* originally TFM_TIME_EXTEND, transform changes */
  if (ELEM(t->mode, TFM_SEQ_SLIDE, TFM_TIME_TRANSLATE)) {
    /* Special annoying case here, need to calc metas with TFM_TIME_EXTEND only */

    /* calc all meta's then effects [#27953] */
    for (seq = seqbasep->first; seq; seq = seq->next) {
      if (seq->type == SEQ_TYPE_META && seq->flag & SELECT) {
        BKE_sequence_calc(t->scene, seq);
      }
    }
    for (seq = seqbasep->first; seq; seq = seq->next) {
      if (seq->seq1 || seq->seq2 || seq->seq3) {
        BKE_sequence_calc(t->scene, seq);
      }
    }

    /* update effects inside meta's */
    for (a = 0, seq_prev = NULL, td = tc->data, td2d = tc->data_2d; a < tc->data_len;
         a++, td++, td2d++, seq_prev = seq) {
      tdsq = (TransDataSeq *)td->extra;
      seq = tdsq->seq;
      if ((seq != seq_prev) && (seq->depth != 0)) {
        if (seq->seq1 || seq->seq2 || seq->seq3) {
          BKE_sequence_calc(t->scene, seq);
        }
      }
    }
  }

  /* need to do the overlap check in a new loop otherwise adjacent strips
   * will not be updated and we'll get false positives */
  seq_prev = NULL;
  for (a = 0, td = tc->data, td2d = tc->data_2d; a < tc->data_len; a++, td++, td2d++) {

    tdsq = (TransDataSeq *)td->extra;
    seq = tdsq->seq;

    if (seq != seq_prev) {
      if (seq->depth == 0) {
        /* test overlap, displays red outline */
        seq->flag &= ~SEQ_OVERLAP;
        if (BKE_sequence_test_overlap(seqbasep, seq)) {
          seq->flag |= SEQ_OVERLAP;
        }
      }
    }
    seq_prev = seq;
  }
}

/* ********************* UV ****************** */

void flushTransUVs(TransInfo *t)
{
  SpaceImage *sima = t->sa->spacedata.first;
  const bool use_pixel_snap = ((sima->pixel_snap_mode != SI_PIXEL_SNAP_DISABLED) &&
                               (t->state != TRANS_CANCEL));

  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    TransData2D *td;
    int a;
    float aspect_inv[2], size[2];

    aspect_inv[0] = 1.0f / t->aspect[0];
    aspect_inv[1] = 1.0f / t->aspect[1];

    if (use_pixel_snap) {
      int size_i[2];
      ED_space_image_get_size(sima, &size_i[0], &size_i[1]);
      size[0] = size_i[0];
      size[1] = size_i[1];
    }

    /* flush to 2d vector from internally used 3d vector */
    for (a = 0, td = tc->data_2d; a < tc->data_len; a++, td++) {
      td->loc2d[0] = td->loc[0] * aspect_inv[0];
      td->loc2d[1] = td->loc[1] * aspect_inv[1];

      if (use_pixel_snap) {
        td->loc2d[0] *= size[0];
        td->loc2d[1] *= size[1];

        switch (sima->pixel_snap_mode) {
          case SI_PIXEL_SNAP_CENTER:
            td->loc2d[0] = roundf(td->loc2d[0] - 0.5f) + 0.5f;
            td->loc2d[1] = roundf(td->loc2d[1] - 0.5f) + 0.5f;
            break;
          case SI_PIXEL_SNAP_CORNER:
            td->loc2d[0] = roundf(td->loc2d[0]);
            td->loc2d[1] = roundf(td->loc2d[1]);
            break;
        }

        td->loc2d[0] /= size[0];
        td->loc2d[1] /= size[1];
      }
    }
  }
}

bool clipUVTransform(TransInfo *t, float vec[2], const bool resize)
{
  bool clipx = true, clipy = true;
  float min[2], max[2];

  min[0] = min[1] = 0.0f;
  max[0] = t->aspect[0];
  max[1] = t->aspect[1];

  FOREACH_TRANS_DATA_CONTAINER (t, tc) {

    TransData *td;
    int a;

    for (a = 0, td = tc->data; a < tc->data_len; a++, td++) {
      minmax_v2v2_v2(min, max, td->loc);
    }
  }

  if (resize) {
    if (min[0] < 0.0f && t->center_global[0] > 0.0f && t->center_global[0] < t->aspect[0] * 0.5f) {
      vec[0] *= t->center_global[0] / (t->center_global[0] - min[0]);
    }
    else if (max[0] > t->aspect[0] && t->center_global[0] < t->aspect[0]) {
      vec[0] *= (t->center_global[0] - t->aspect[0]) / (t->center_global[0] - max[0]);
    }
    else {
      clipx = 0;
    }

    if (min[1] < 0.0f && t->center_global[1] > 0.0f && t->center_global[1] < t->aspect[1] * 0.5f) {
      vec[1] *= t->center_global[1] / (t->center_global[1] - min[1]);
    }
    else if (max[1] > t->aspect[1] && t->center_global[1] < t->aspect[1]) {
      vec[1] *= (t->center_global[1] - t->aspect[1]) / (t->center_global[1] - max[1]);
    }
    else {
      clipy = 0;
    }
  }
  else {
    if (min[0] < 0.0f) {
      vec[0] -= min[0];
    }
    else if (max[0] > t->aspect[0]) {
      vec[0] -= max[0] - t->aspect[0];
    }
    else {
      clipx = 0;
    }

    if (min[1] < 0.0f) {
      vec[1] -= min[1];
    }
    else if (max[1] > t->aspect[1]) {
      vec[1] -= max[1] - t->aspect[1];
    }
    else {
      clipy = 0;
    }
  }

  return (clipx || clipy);
}

void clipUVData(TransInfo *t)
{
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    TransData *td = tc->data;
    for (int a = 0; a < tc->data_len; a++, td++) {
      if (td->flag & TD_NOACTION) {
        break;
      }

      if ((td->flag & TD_SKIP) || (!td->loc)) {
        continue;
      }

      td->loc[0] = min_ff(max_ff(0.0f, td->loc[0]), t->aspect[0]);
      td->loc[1] = min_ff(max_ff(0.0f, td->loc[1]), t->aspect[1]);
    }
  }
}

/* ********************* ANIMATION EDITORS (GENERAL) ************************* */

/* This function tests if a point is on the "mouse" side of the cursor/frame-marking */
bool FrameOnMouseSide(char side, float frame, float cframe)
{
  /* both sides, so it doesn't matter */
  if (side == 'B') {
    return true;
  }

  /* only on the named side */
  if (side == 'R') {
    return (frame >= cframe);
  }
  else {
    return (frame <= cframe);
  }
}

/* ********************* ACTION EDITOR ****************** */

static int gpf_cmp_frame(void *thunk, const void *a, const void *b)
{
  const bGPDframe *frame_a = a;
  const bGPDframe *frame_b = b;

  if (frame_a->framenum < frame_b->framenum) {
    return -1;
  }
  if (frame_a->framenum > frame_b->framenum) {
    return 1;
  }
  *((bool *)thunk) = true;
  /* selected last */
  if ((frame_a->flag & GP_FRAME_SELECT) && ((frame_b->flag & GP_FRAME_SELECT) == 0)) {
    return 1;
  }
  return 0;
}

static int masklay_shape_cmp_frame(void *thunk, const void *a, const void *b)
{
  const MaskLayerShape *frame_a = a;
  const MaskLayerShape *frame_b = b;

  if (frame_a->frame < frame_b->frame) {
    return -1;
  }
  if (frame_a->frame > frame_b->frame) {
    return 1;
  }
  *((bool *)thunk) = true;
  /* selected last */
  if ((frame_a->flag & MASK_SHAPE_SELECT) && ((frame_b->flag & MASK_SHAPE_SELECT) == 0)) {
    return 1;
  }
  return 0;
}

/* Called by special_aftertrans_update to make sure selected gp-frames replace
 * any other gp-frames which may reside on that frame (that are not selected).
 * It also makes sure gp-frames are still stored in chronological order after
 * transform.
 */
static void posttrans_gpd_clean(bGPdata *gpd)
{
  bGPDlayer *gpl;

  for (gpl = gpd->layers.first; gpl; gpl = gpl->next) {
    bGPDframe *gpf, *gpfn;
    bool is_double = false;

    BLI_listbase_sort_r(&gpl->frames, gpf_cmp_frame, &is_double);

    if (is_double) {
      for (gpf = gpl->frames.first; gpf; gpf = gpfn) {
        gpfn = gpf->next;
        if (gpfn && gpf->framenum == gpfn->framenum) {
          BKE_gpencil_layer_delframe(gpl, gpf);
        }
      }
    }

#ifdef DEBUG
    for (gpf = gpl->frames.first; gpf; gpf = gpf->next) {
      BLI_assert(!gpf->next || gpf->framenum < gpf->next->framenum);
    }
#endif
  }
  /* set cache flag to dirty */
  DEG_id_tag_update(&gpd->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
}

static void posttrans_mask_clean(Mask *mask)
{
  MaskLayer *masklay;

  for (masklay = mask->masklayers.first; masklay; masklay = masklay->next) {
    MaskLayerShape *masklay_shape, *masklay_shape_next;
    bool is_double = false;

    BLI_listbase_sort_r(&masklay->splines_shapes, masklay_shape_cmp_frame, &is_double);

    if (is_double) {
      for (masklay_shape = masklay->splines_shapes.first; masklay_shape;
           masklay_shape = masklay_shape_next) {
        masklay_shape_next = masklay_shape->next;
        if (masklay_shape_next && masklay_shape->frame == masklay_shape_next->frame) {
          BKE_mask_layer_shape_unlink(masklay, masklay_shape);
        }
      }
    }

#ifdef DEBUG
    for (masklay_shape = masklay->splines_shapes.first; masklay_shape;
         masklay_shape = masklay_shape->next) {
      BLI_assert(!masklay_shape->next || masklay_shape->frame < masklay_shape->next->frame);
    }
#endif
  }
}

/* Time + Average value */
typedef struct tRetainedKeyframe {
  struct tRetainedKeyframe *next, *prev;
  float frame; /* frame to cluster around */
  float val;   /* average value */

  size_t tot_count; /* number of keyframes that have been averaged */
  size_t del_count; /* number of keyframes of this sort that have been deleted so far */
} tRetainedKeyframe;

/* Called during special_aftertrans_update to make sure selected keyframes replace
 * any other keyframes which may reside on that frame (that is not selected).
 */
static void posttrans_fcurve_clean(FCurve *fcu, const bool use_handle)
{
  /* NOTE: We assume that all keys are sorted */
  ListBase retained_keys = {NULL, NULL};
  const bool can_average_points = ((fcu->flag & (FCURVE_INT_VALUES | FCURVE_DISCRETE_VALUES)) ==
                                   0);

  /* sanity checks */
  if ((fcu->totvert == 0) || (fcu->bezt == NULL)) {
    return;
  }

  /* 1) Identify selected keyframes, and average the values on those
   * in case there are collisions due to multiple keys getting scaled
   * to all end up on the same frame
   */
  for (int i = 0; i < fcu->totvert; i++) {
    BezTriple *bezt = &fcu->bezt[i];

    if (BEZT_ISSEL_ANY(bezt)) {
      bool found = false;

      /* If there's another selected frame here, merge it */
      for (tRetainedKeyframe *rk = retained_keys.last; rk; rk = rk->prev) {
        if (IS_EQT(rk->frame, bezt->vec[1][0], BEZT_BINARYSEARCH_THRESH)) {
          rk->val += bezt->vec[1][1];
          rk->tot_count++;

          found = true;
          break;
        }
        else if (rk->frame < bezt->vec[1][0]) {
          /* Terminate early if have passed the supposed insertion point? */
          break;
        }
      }

      /* If nothing found yet, create a new one */
      if (found == false) {
        tRetainedKeyframe *rk = MEM_callocN(sizeof(tRetainedKeyframe), "tRetainedKeyframe");

        rk->frame = bezt->vec[1][0];
        rk->val = bezt->vec[1][1];
        rk->tot_count = 1;

        BLI_addtail(&retained_keys, rk);
      }
    }
  }

  if (BLI_listbase_is_empty(&retained_keys)) {
    /* This may happen if none of the points were selected... */
    if (G.debug & G_DEBUG) {
      printf("%s: nothing to do for FCurve %p (rna_path = '%s')\n", __func__, fcu, fcu->rna_path);
    }
    return;
  }
  else {
    /* Compute the average values for each retained keyframe */
    for (tRetainedKeyframe *rk = retained_keys.first; rk; rk = rk->next) {
      rk->val = rk->val / (float)rk->tot_count;
    }
  }

  /* 2) Delete all keyframes duplicating the "retained keys" found above
   *   - Most of these will be unselected keyframes
   *   - Some will be selected keyframes though. For those, we only keep the last one
   *     (or else everything is gone), and replace its value with the averaged value.
   */
  for (int i = fcu->totvert - 1; i >= 0; i--) {
    BezTriple *bezt = &fcu->bezt[i];

    /* Is this keyframe a candidate for deletion? */
    /* TODO: Replace loop with an O(1) lookup instead */
    for (tRetainedKeyframe *rk = retained_keys.last; rk; rk = rk->prev) {
      if (IS_EQT(bezt->vec[1][0], rk->frame, BEZT_BINARYSEARCH_THRESH)) {
        /* Selected keys are treated with greater care than unselected ones... */
        if (BEZT_ISSEL_ANY(bezt)) {
          /* - If this is the last selected key left (based on rk->del_count) ==> UPDATE IT
           *   (or else we wouldn't have any keyframe left here)
           * - Otherwise, there are still other selected keyframes on this frame
           *   to be merged down still ==> DELETE IT
           */
          if (rk->del_count == rk->tot_count - 1) {
            /* Update keyframe... */
            if (can_average_points) {
              /* TODO: update handles too? */
              bezt->vec[1][1] = rk->val;
            }
          }
          else {
            /* Delete Keyframe */
            delete_fcurve_key(fcu, i, 0);
          }

          /* Update count of how many we've deleted
           * - It should only matter that we're doing this for all but the last one
           */
          rk->del_count++;
        }
        else {
          /* Always delete - Unselected keys don't matter */
          delete_fcurve_key(fcu, i, 0);
        }

        /* Stop the RK search... we've found our match now */
        break;
      }
    }
  }

  /* 3) Recalculate handles */
  testhandles_fcurve(fcu, use_handle);

  /* cleanup */
  BLI_freelistN(&retained_keys);
}

/* Called by special_aftertrans_update to make sure selected keyframes replace
 * any other keyframes which may reside on that frame (that is not selected).
 * remake_action_ipos should have already been called
 */
static void posttrans_action_clean(bAnimContext *ac, bAction *act)
{
  ListBase anim_data = {NULL, NULL};
  bAnimListElem *ale;
  int filter;

  /* filter data */
  filter = (ANIMFILTER_DATA_VISIBLE | ANIMFILTER_FOREDIT /*| ANIMFILTER_CURVESONLY*/);
  ANIM_animdata_filter(ac, &anim_data, filter, act, ANIMCONT_ACTION);

  /* loop through relevant data, removing keyframes as appropriate
   *      - all keyframes are converted in/out of global time
   */
  for (ale = anim_data.first; ale; ale = ale->next) {
    AnimData *adt = ANIM_nla_mapping_get(ac, ale);

    if (adt) {
      ANIM_nla_mapping_apply_fcurve(adt, ale->key_data, 0, 0);
      posttrans_fcurve_clean(ale->key_data, false); /* only use handles in graph editor */
      ANIM_nla_mapping_apply_fcurve(adt, ale->key_data, 1, 0);
    }
    else {
      posttrans_fcurve_clean(ale->key_data, false); /* only use handles in graph editor */
    }
  }

  /* free temp data */
  ANIM_animdata_freelist(&anim_data);
}

/* ----------------------------- */

/* This function helps flush transdata written to tempdata into the gp-frames  */
void flushTransIntFrameActionData(TransInfo *t)
{
  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);
  tGPFtransdata *tfd = tc->custom.type.data;

  /* flush data! */
  for (int i = 0; i < tc->data_len; i++, tfd++) {
    *(tfd->sdata) = round_fl_to_int(tfd->val);
  }
}

/* ********************* GRAPH EDITOR ************************* */

/* struct for use in re-sorting BezTriples during Graph Editor transform */
typedef struct BeztMap {
  BezTriple *bezt;
  unsigned int oldIndex; /* index of bezt in fcu->bezt array before sorting */
  unsigned int newIndex; /* index of bezt in fcu->bezt array after sorting */
  short swapHs;          /* swap order of handles (-1=clear; 0=not checked, 1=swap) */
  char pipo, cipo;       /* interpolation of current and next segments */
} BeztMap;

/* This function converts an FCurve's BezTriple array to a BeztMap array
 * NOTE: this allocates memory that will need to get freed later
 */
static BeztMap *bezt_to_beztmaps(BezTriple *bezts, int totvert, const short UNUSED(use_handle))
{
  BezTriple *bezt = bezts;
  BezTriple *prevbezt = NULL;
  BeztMap *bezm, *bezms;
  int i;

  /* allocate memory for this array */
  if (totvert == 0 || bezts == NULL) {
    return NULL;
  }
  bezm = bezms = MEM_callocN(sizeof(BeztMap) * totvert, "BeztMaps");

  /* assign beztriples to beztmaps */
  for (i = 0; i < totvert; i++, bezm++, prevbezt = bezt, bezt++) {
    bezm->bezt = bezt;

    bezm->oldIndex = i;
    bezm->newIndex = i;

    bezm->pipo = (prevbezt) ? prevbezt->ipo : bezt->ipo;
    bezm->cipo = bezt->ipo;
  }

  return bezms;
}

/* This function copies the code of sort_time_ipocurve, but acts on BeztMap structs instead */
static void sort_time_beztmaps(BeztMap *bezms, int totvert, const short UNUSED(use_handle))
{
  BeztMap *bezm;
  int i, ok = 1;

  /* keep repeating the process until nothing is out of place anymore */
  while (ok) {
    ok = 0;

    bezm = bezms;
    i = totvert;
    while (i--) {
      /* is current bezm out of order (i.e. occurs later than next)? */
      if (i > 0) {
        if (bezm->bezt->vec[1][0] > (bezm + 1)->bezt->vec[1][0]) {
          bezm->newIndex++;
          (bezm + 1)->newIndex--;

          SWAP(BeztMap, *bezm, *(bezm + 1));

          ok = 1;
        }
      }

      /* do we need to check if the handles need to be swapped?
       * optimization: this only needs to be performed in the first loop
       */
      if (bezm->swapHs == 0) {
        if ((bezm->bezt->vec[0][0] > bezm->bezt->vec[1][0]) &&
            (bezm->bezt->vec[2][0] < bezm->bezt->vec[1][0])) {
          /* handles need to be swapped */
          bezm->swapHs = 1;
        }
        else {
          /* handles need to be cleared */
          bezm->swapHs = -1;
        }
      }

      bezm++;
    }
  }
}

/* This function firstly adjusts the pointers that the transdata has to each BezTriple */
static void beztmap_to_data(
    TransInfo *t, FCurve *fcu, BeztMap *bezms, int totvert, const short UNUSED(use_handle))
{
  BezTriple *bezts = fcu->bezt;
  BeztMap *bezm;
  TransData2D *td2d;
  TransData *td;
  int i, j;
  char *adjusted;

  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);

  /* dynamically allocate an array of chars to mark whether an TransData's
   * pointers have been fixed already, so that we don't override ones that are
   * already done
   */
  adjusted = MEM_callocN(tc->data_len, "beztmap_adjusted_map");

  /* for each beztmap item, find if it is used anywhere */
  bezm = bezms;
  for (i = 0; i < totvert; i++, bezm++) {
    /* loop through transdata, testing if we have a hit
     * for the handles (vec[0]/vec[2]), we must also check if they need to be swapped...
     */
    td2d = tc->data_2d;
    td = tc->data;
    for (j = 0; j < tc->data_len; j++, td2d++, td++) {
      /* skip item if already marked */
      if (adjusted[j] != 0) {
        continue;
      }

      /* update all transdata pointers, no need to check for selections etc,
       * since only points that are really needed were created as transdata
       */
      if (td2d->loc2d == bezm->bezt->vec[0]) {
        if (bezm->swapHs == 1) {
          td2d->loc2d = (bezts + bezm->newIndex)->vec[2];
        }
        else {
          td2d->loc2d = (bezts + bezm->newIndex)->vec[0];
        }
        adjusted[j] = 1;
      }
      else if (td2d->loc2d == bezm->bezt->vec[2]) {
        if (bezm->swapHs == 1) {
          td2d->loc2d = (bezts + bezm->newIndex)->vec[0];
        }
        else {
          td2d->loc2d = (bezts + bezm->newIndex)->vec[2];
        }
        adjusted[j] = 1;
      }
      else if (td2d->loc2d == bezm->bezt->vec[1]) {
        td2d->loc2d = (bezts + bezm->newIndex)->vec[1];

        /* if only control point is selected, the handle pointers need to be updated as well */
        if (td2d->h1) {
          td2d->h1 = (bezts + bezm->newIndex)->vec[0];
        }
        if (td2d->h2) {
          td2d->h2 = (bezts + bezm->newIndex)->vec[2];
        }

        adjusted[j] = 1;
      }

      /* the handle type pointer has to be updated too */
      if (adjusted[j] && td->flag & TD_BEZTRIPLE && td->hdata) {
        if (bezm->swapHs == 1) {
          td->hdata->h1 = &(bezts + bezm->newIndex)->h2;
          td->hdata->h2 = &(bezts + bezm->newIndex)->h1;
        }
        else {
          td->hdata->h1 = &(bezts + bezm->newIndex)->h1;
          td->hdata->h2 = &(bezts + bezm->newIndex)->h2;
        }
      }
    }
  }

  /* free temp memory used for 'adjusted' array */
  MEM_freeN(adjusted);
}

/* This function is called by recalcData during the Transform loop to recalculate
 * the handles of curves and sort the keyframes so that the curves draw correctly.
 * It is only called if some keyframes have moved out of order.
 *
 * anim_data is the list of channels (F-Curves) retrieved already containing the
 * channels to work on. It should not be freed here as it may still need to be used.
 */
void remake_graph_transdata(TransInfo *t, ListBase *anim_data)
{
  SpaceGraph *sipo = (SpaceGraph *)t->sa->spacedata.first;
  bAnimListElem *ale;
  const bool use_handle = (sipo->flag & SIPO_NOHANDLES) == 0;

  /* sort and reassign verts */
  for (ale = anim_data->first; ale; ale = ale->next) {
    FCurve *fcu = (FCurve *)ale->key_data;

    if (fcu->bezt) {
      BeztMap *bezm;

      /* adjust transform-data pointers */
      /* note, none of these functions use 'use_handle', it could be removed */
      bezm = bezt_to_beztmaps(fcu->bezt, fcu->totvert, use_handle);
      sort_time_beztmaps(bezm, fcu->totvert, use_handle);
      beztmap_to_data(t, fcu, bezm, fcu->totvert, use_handle);

      /* free mapping stuff */
      MEM_freeN(bezm);

      /* re-sort actual beztriples (perhaps this could be done using the beztmaps to save time?) */
      sort_time_fcurve(fcu);

      /* make sure handles are all set correctly */
      testhandles_fcurve(fcu, use_handle);
    }
  }
}

/* this function is called on recalcData to apply the transforms applied
 * to the transdata on to the actual keyframe data
 */
void flushTransGraphData(TransInfo *t)
{
  SpaceGraph *sipo = (SpaceGraph *)t->sa->spacedata.first;
  TransData *td;
  TransData2D *td2d;
  TransDataGraph *tdg;
  Scene *scene = t->scene;
  double secf = FPS;
  int a;

  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);

  /* flush to 2d vector from internally used 3d vector */
  for (a = 0, td = tc->data, td2d = tc->data_2d, tdg = tc->custom.type.data; a < tc->data_len;
       a++, td++, td2d++, tdg++) {
    /* pointers to relevant AnimData blocks are stored in the td->extra pointers */
    AnimData *adt = (AnimData *)td->extra;

    float inv_unit_scale = 1.0f / tdg->unit_scale;

    /* handle snapping for time values
     * - we should still be in NLA-mapping timespace
     * - only apply to keyframes (but never to handles)
     * - don't do this when canceling, or else these changes won't go away
     */
    if ((t->state != TRANS_CANCEL) && (td->flag & TD_NOTIMESNAP) == 0) {
      switch (sipo->autosnap) {
        case SACTSNAP_FRAME: /* snap to nearest frame */
          td2d->loc[0] = floor((double)td2d->loc[0] + 0.5);
          break;

        case SACTSNAP_SECOND: /* snap to nearest second */
          td2d->loc[0] = floor(((double)td2d->loc[0] / secf) + 0.5) * secf;
          break;

        case SACTSNAP_MARKER: /* snap to nearest marker */
          td2d->loc[0] = (float)ED_markers_find_nearest_marker_time(&t->scene->markers,
                                                                    td2d->loc[0]);
          break;
      }
    }

    /* we need to unapply the nla-mapping from the time in some situations */
    if (adt) {
      td2d->loc2d[0] = BKE_nla_tweakedit_remap(adt, td2d->loc[0], NLATIME_CONVERT_UNMAP);
    }
    else {
      td2d->loc2d[0] = td2d->loc[0];
    }

    /** Time-stepping auto-snapping modes don't get applied for Graph Editor transforms,
     * as these use the generic transform modes which don't account for this sort of thing.
     * These ones aren't affected by NLA mapping, so we do this after the conversion...
     *
     * \note We also have to apply to td->loc,
     * as that's what the handle-adjustment step below looks to,
     * otherwise we get "swimming handles".
     *
     * \note We don't do this when canceling transforms, or else these changes don't go away.
     */
    if ((t->state != TRANS_CANCEL) && (td->flag & TD_NOTIMESNAP) == 0 &&
        ELEM(sipo->autosnap, SACTSNAP_STEP, SACTSNAP_TSTEP)) {
      switch (sipo->autosnap) {
        case SACTSNAP_STEP: /* frame step */
          td2d->loc2d[0] = floor((double)td2d->loc[0] + 0.5);
          td->loc[0] = floor((double)td->loc[0] + 0.5);
          break;

        case SACTSNAP_TSTEP: /* second step */
          /* XXX: the handle behavior in this case is still not quite right... */
          td2d->loc[0] = floor(((double)td2d->loc[0] / secf) + 0.5) * secf;
          td->loc[0] = floor(((double)td->loc[0] / secf) + 0.5) * secf;
          break;
      }
    }

    /* if int-values only, truncate to integers */
    if (td->flag & TD_INTVALUES) {
      td2d->loc2d[1] = floorf(td2d->loc[1] * inv_unit_scale - tdg->offset + 0.5f);
    }
    else {
      td2d->loc2d[1] = td2d->loc[1] * inv_unit_scale - tdg->offset;
    }

    if ((td->flag & TD_MOVEHANDLE1) && td2d->h1) {
      td2d->h1[0] = td2d->ih1[0] + td->loc[0] - td->iloc[0];
      td2d->h1[1] = td2d->ih1[1] + (td->loc[1] - td->iloc[1]) * inv_unit_scale;
    }

    if ((td->flag & TD_MOVEHANDLE2) && td2d->h2) {
      td2d->h2[0] = td2d->ih2[0] + td->loc[0] - td->iloc[0];
      td2d->h2[1] = td2d->ih2[1] + (td->loc[1] - td->iloc[1]) * inv_unit_scale;
    }
  }
}

/* *********************** Transform data ******************* */

/* Little helper function for ObjectToTransData used to give certain
 * constraints (ChildOf, FollowPath, and others that may be added)
 * inverse corrections for transform, so that they aren't in CrazySpace.
 * These particular constraints benefit from this, but others don't, hence
 * this semi-hack ;-)    - Aligorith
 */
bool constraints_list_needinv(TransInfo *t, ListBase *list)
{
  bConstraint *con;

  /* loop through constraints, checking if there's one of the mentioned
   * constraints needing special crazyspace corrections
   */
  if (list) {
    for (con = list->first; con; con = con->next) {
      /* only consider constraint if it is enabled, and has influence on result */
      if ((con->flag & CONSTRAINT_DISABLE) == 0 && (con->enforce != 0.0f)) {
        /* (affirmative) returns for specific constraints here... */
        /* constraints that require this regardless  */
        if (ELEM(con->type,
                 CONSTRAINT_TYPE_FOLLOWPATH,
                 CONSTRAINT_TYPE_CLAMPTO,
                 CONSTRAINT_TYPE_ARMATURE,
                 CONSTRAINT_TYPE_OBJECTSOLVER,
                 CONSTRAINT_TYPE_FOLLOWTRACK)) {
          return true;
        }

        /* constraints that require this only under special conditions */
        if (con->type == CONSTRAINT_TYPE_CHILDOF) {
          /* ChildOf constraint only works when using all location components, see T42256. */
          bChildOfConstraint *data = (bChildOfConstraint *)con->data;

          if ((data->flag & CHILDOF_LOCX) && (data->flag & CHILDOF_LOCY) &&
              (data->flag & CHILDOF_LOCZ)) {
            return true;
          }
        }
        else if (con->type == CONSTRAINT_TYPE_ROTLIKE) {
          /* CopyRot constraint only does this when rotating, and offset is on */
          bRotateLikeConstraint *data = (bRotateLikeConstraint *)con->data;

          if ((data->flag & ROTLIKE_OFFSET) && (t->mode == TFM_ROTATION)) {
            return true;
          }
        }
        else if (con->type == CONSTRAINT_TYPE_TRANSFORM) {
          /* Transform constraint needs it for rotation at least (r.57309),
           * but doing so when translating may also mess things up [#36203]
           */

          if (t->mode == TFM_ROTATION) {
            return true;
          }
          /* ??? (t->mode == TFM_SCALE) ? */
        }
      }
    }
  }

  /* no appropriate candidates found */
  return false;
}

/**
 * Auto-keyframing feature - for objects
 *
 * \param tmode: A transform mode.
 *
 * \note Context may not always be available,
 * so must check before using it as it's a luxury for a few cases.
 */
void autokeyframe_object(bContext *C, Scene *scene, ViewLayer *view_layer, Object *ob, int tmode)
{
  Main *bmain = CTX_data_main(C);
  ID *id = &ob->id;
  FCurve *fcu;

  // TODO: this should probably be done per channel instead...
  if (autokeyframe_cfra_can_key(scene, id)) {
    ReportList *reports = CTX_wm_reports(C);
    ToolSettings *ts = scene->toolsettings;
    KeyingSet *active_ks = ANIM_scene_get_active_keyingset(scene);
    ListBase dsources = {NULL, NULL};
    float cfra = (float)CFRA;  // xxx this will do for now
    short flag = 0;

    /* get flags used for inserting keyframes */
    flag = ANIM_get_keyframing_flags(scene, 1);

    /* add datasource override for the object */
    ANIM_relative_keyingset_add_source(&dsources, id, NULL, NULL);

    if (IS_AUTOKEY_FLAG(scene, ONLYKEYINGSET) && (active_ks)) {
      /* Only insert into active keyingset
       * NOTE: we assume here that the active Keying Set
       * does not need to have its iterator overridden.
       */
      ANIM_apply_keyingset(C, &dsources, NULL, active_ks, MODIFYKEY_MODE_INSERT, cfra);
    }
    else if (IS_AUTOKEY_FLAG(scene, INSERTAVAIL)) {
      AnimData *adt = ob->adt;

      /* only key on available channels */
      if (adt && adt->action) {
        ListBase nla_cache = {NULL, NULL};
        for (fcu = adt->action->curves.first; fcu; fcu = fcu->next) {
          fcu->flag &= ~FCURVE_SELECTED;
          insert_keyframe(bmain,
                          reports,
                          id,
                          adt->action,
                          (fcu->grp ? fcu->grp->name : NULL),
                          fcu->rna_path,
                          fcu->array_index,
                          cfra,
                          ts->keyframe_type,
                          &nla_cache,
                          flag);
        }

        BKE_animsys_free_nla_keyframing_context_cache(&nla_cache);
      }
    }
    else if (IS_AUTOKEY_FLAG(scene, INSERTNEEDED)) {
      bool do_loc = false, do_rot = false, do_scale = false;

      /* filter the conditions when this happens (assume that curarea->spacetype==SPACE_VIE3D) */
      if (tmode == TFM_TRANSLATION) {
        do_loc = true;
      }
      else if (ELEM(tmode, TFM_ROTATION, TFM_TRACKBALL)) {
        if (scene->toolsettings->transform_pivot_point == V3D_AROUND_ACTIVE) {
          if (ob != OBACT(view_layer)) {
            do_loc = true;
          }
        }
        else if (scene->toolsettings->transform_pivot_point == V3D_AROUND_CURSOR) {
          do_loc = true;
        }

        if ((scene->toolsettings->transform_flag & SCE_XFORM_AXIS_ALIGN) == 0) {
          do_rot = true;
        }
      }
      else if (tmode == TFM_RESIZE) {
        if (scene->toolsettings->transform_pivot_point == V3D_AROUND_ACTIVE) {
          if (ob != OBACT(view_layer)) {
            do_loc = true;
          }
        }
        else if (scene->toolsettings->transform_pivot_point == V3D_AROUND_CURSOR) {
          do_loc = true;
        }

        if ((scene->toolsettings->transform_flag & SCE_XFORM_AXIS_ALIGN) == 0) {
          do_scale = true;
        }
      }

      /* insert keyframes for the affected sets of channels using the builtin KeyingSets found */
      if (do_loc) {
        KeyingSet *ks = ANIM_builtin_keyingset_get_named(NULL, ANIM_KS_LOCATION_ID);
        ANIM_apply_keyingset(C, &dsources, NULL, ks, MODIFYKEY_MODE_INSERT, cfra);
      }
      if (do_rot) {
        KeyingSet *ks = ANIM_builtin_keyingset_get_named(NULL, ANIM_KS_ROTATION_ID);
        ANIM_apply_keyingset(C, &dsources, NULL, ks, MODIFYKEY_MODE_INSERT, cfra);
      }
      if (do_scale) {
        KeyingSet *ks = ANIM_builtin_keyingset_get_named(NULL, ANIM_KS_SCALING_ID);
        ANIM_apply_keyingset(C, &dsources, NULL, ks, MODIFYKEY_MODE_INSERT, cfra);
      }
    }
    /* insert keyframe in all (transform) channels */
    else {
      KeyingSet *ks = ANIM_builtin_keyingset_get_named(NULL, ANIM_KS_LOC_ROT_SCALE_ID);
      ANIM_apply_keyingset(C, &dsources, NULL, ks, MODIFYKEY_MODE_INSERT, cfra);
    }

    /* free temp info */
    BLI_freelistN(&dsources);
  }
}

/* Return if we need to update motion paths, only if they already exist,
 * and we will insert a keyframe at the end of transform. */
bool motionpath_need_update_object(Scene *scene, Object *ob)
{
  /* XXX: there's potential here for problems with unkeyed rotations/scale,
   *      but for now (until proper data-locality for baking operations),
   *      this should be a better fix for T24451 and T37755
   */

  if (autokeyframe_cfra_can_key(scene, &ob->id)) {
    return (ob->avs.path_bakeflag & MOTIONPATH_BAKE_HAS_PATHS) != 0;
  }

  return false;
}

/**
 * Auto-keyframing feature - for poses/pose-channels
 *
 * \param tmode: A transform mode.
 *
 * targetless_ik: has targetless ik been done on any channels?
 *
 * \note Context may not always be available,
 * so must check before using it as it's a luxury for a few cases.
 */
void autokeyframe_pose(bContext *C, Scene *scene, Object *ob, int tmode, short targetless_ik)
{
  Main *bmain = CTX_data_main(C);
  ID *id = &ob->id;
  AnimData *adt = ob->adt;
  bAction *act = (adt) ? adt->action : NULL;
  bPose *pose = ob->pose;
  bPoseChannel *pchan;
  FCurve *fcu;

  // TODO: this should probably be done per channel instead...
  if (autokeyframe_cfra_can_key(scene, id)) {
    ReportList *reports = CTX_wm_reports(C);
    ToolSettings *ts = scene->toolsettings;
    KeyingSet *active_ks = ANIM_scene_get_active_keyingset(scene);
    ListBase nla_cache = {NULL, NULL};
    float cfra = (float)CFRA;
    short flag = 0;

    /* flag is initialized from UserPref keyframing settings
     * - special exception for targetless IK - INSERTKEY_MATRIX keyframes should get
     *   visual keyframes even if flag not set, as it's not that useful otherwise
     *   (for quick animation recording)
     */
    flag = ANIM_get_keyframing_flags(scene, 1);

    if (targetless_ik) {
      flag |= INSERTKEY_MATRIX;
    }

    for (pchan = pose->chanbase.first; pchan; pchan = pchan->next) {
      if (pchan->bone->flag & (BONE_TRANSFORM | BONE_TRANSFORM_MIRROR)) {

        ListBase dsources = {NULL, NULL};

        /* clear any 'unkeyed' flag it may have */
        pchan->bone->flag &= ~BONE_UNKEYED;

        /* add datasource override for the camera object */
        ANIM_relative_keyingset_add_source(&dsources, id, &RNA_PoseBone, pchan);

        /* only insert into active keyingset? */
        if (IS_AUTOKEY_FLAG(scene, ONLYKEYINGSET) && (active_ks)) {
          /* run the active Keying Set on the current datasource */
          ANIM_apply_keyingset(C, &dsources, NULL, active_ks, MODIFYKEY_MODE_INSERT, cfra);
        }
        /* only insert into available channels? */
        else if (IS_AUTOKEY_FLAG(scene, INSERTAVAIL)) {
          if (act) {
            for (fcu = act->curves.first; fcu; fcu = fcu->next) {
              /* only insert keyframes for this F-Curve if it affects the current bone */
              if (strstr(fcu->rna_path, "bones")) {
                char *pchanName = BLI_str_quoted_substrN(fcu->rna_path, "bones[");

                /* only if bone name matches too...
                 * NOTE: this will do constraints too, but those are ok to do here too?
                 */
                if (pchanName && STREQ(pchanName, pchan->name)) {
                  insert_keyframe(bmain,
                                  reports,
                                  id,
                                  act,
                                  ((fcu->grp) ? (fcu->grp->name) : (NULL)),
                                  fcu->rna_path,
                                  fcu->array_index,
                                  cfra,
                                  ts->keyframe_type,
                                  &nla_cache,
                                  flag);
                }

                if (pchanName) {
                  MEM_freeN(pchanName);
                }
              }
            }
          }
        }
        /* only insert keyframe if needed? */
        else if (IS_AUTOKEY_FLAG(scene, INSERTNEEDED)) {
          bool do_loc = false, do_rot = false, do_scale = false;

          /* Filter the conditions when this happens
           * (assume that 'curarea->spacetype == SPACE_VIEW3D'). */
          if (tmode == TFM_TRANSLATION) {
            if (targetless_ik) {
              do_rot = true;
            }
            else {
              do_loc = true;
            }
          }
          else if (ELEM(tmode, TFM_ROTATION, TFM_TRACKBALL)) {
            if (ELEM(scene->toolsettings->transform_pivot_point,
                     V3D_AROUND_CURSOR,
                     V3D_AROUND_ACTIVE)) {
              do_loc = true;
            }

            if ((scene->toolsettings->transform_flag & SCE_XFORM_AXIS_ALIGN) == 0) {
              do_rot = true;
            }
          }
          else if (tmode == TFM_RESIZE) {
            if (ELEM(scene->toolsettings->transform_pivot_point,
                     V3D_AROUND_CURSOR,
                     V3D_AROUND_ACTIVE)) {
              do_loc = true;
            }

            if ((scene->toolsettings->transform_flag & SCE_XFORM_AXIS_ALIGN) == 0) {
              do_scale = true;
            }
          }

          if (do_loc) {
            KeyingSet *ks = ANIM_builtin_keyingset_get_named(NULL, ANIM_KS_LOCATION_ID);
            ANIM_apply_keyingset(C, &dsources, NULL, ks, MODIFYKEY_MODE_INSERT, cfra);
          }
          if (do_rot) {
            KeyingSet *ks = ANIM_builtin_keyingset_get_named(NULL, ANIM_KS_ROTATION_ID);
            ANIM_apply_keyingset(C, &dsources, NULL, ks, MODIFYKEY_MODE_INSERT, cfra);
          }
          if (do_scale) {
            KeyingSet *ks = ANIM_builtin_keyingset_get_named(NULL, ANIM_KS_SCALING_ID);
            ANIM_apply_keyingset(C, &dsources, NULL, ks, MODIFYKEY_MODE_INSERT, cfra);
          }
        }
        /* insert keyframe in all (transform) channels */
        else {
          KeyingSet *ks = ANIM_builtin_keyingset_get_named(NULL, ANIM_KS_LOC_ROT_SCALE_ID);
          ANIM_apply_keyingset(C, &dsources, NULL, ks, MODIFYKEY_MODE_INSERT, cfra);
        }

        /* free temp info */
        BLI_freelistN(&dsources);
      }
    }

    BKE_animsys_free_nla_keyframing_context_cache(&nla_cache);
  }
  else {
    /* tag channels that should have unkeyed data */
    for (pchan = pose->chanbase.first; pchan; pchan = pchan->next) {
      if (pchan->bone->flag & BONE_TRANSFORM) {
        /* tag this channel */
        pchan->bone->flag |= BONE_UNKEYED;
      }
    }
  }
}

/* Return if we need to update motion paths, only if they already exist,
 * and we will insert a keyframe at the end of transform. */
bool motionpath_need_update_pose(Scene *scene, Object *ob)
{
  if (autokeyframe_cfra_can_key(scene, &ob->id)) {
    return (ob->pose->avs.path_bakeflag & MOTIONPATH_BAKE_HAS_PATHS) != 0;
  }

  return false;
}

static void special_aftertrans_update__movieclip(bContext *C, TransInfo *t)
{
  SpaceClip *sc = t->sa->spacedata.first;
  MovieClip *clip = ED_space_clip_get_clip(sc);
  ListBase *plane_tracks_base = BKE_tracking_get_active_plane_tracks(&clip->tracking);
  const int framenr = ED_space_clip_get_clip_frame_number(sc);
  /* Update coordinates of modified plane tracks. */
  for (MovieTrackingPlaneTrack *plane_track = plane_tracks_base->first; plane_track;
       plane_track = plane_track->next) {
    bool do_update = false;
    if (plane_track->flag & PLANE_TRACK_HIDDEN) {
      continue;
    }
    do_update |= PLANE_TRACK_VIEW_SELECTED(plane_track) != 0;
    if (do_update == false) {
      if ((plane_track->flag & PLANE_TRACK_AUTOKEY) == 0) {
        int i;
        for (i = 0; i < plane_track->point_tracksnr; i++) {
          MovieTrackingTrack *track = plane_track->point_tracks[i];
          if (TRACK_VIEW_SELECTED(sc, track)) {
            do_update = true;
            break;
          }
        }
      }
    }
    if (do_update) {
      BKE_tracking_track_plane_from_existing_motion(plane_track, framenr);
    }
  }
  if (t->scene->nodetree != NULL) {
    /* Tracks can be used for stabilization nodes,
     * flush update for such nodes.
     */
    nodeUpdateID(t->scene->nodetree, &clip->id);
    WM_event_add_notifier(C, NC_SCENE | ND_NODES, NULL);
  }
}

static void special_aftertrans_update__mask(bContext *C, TransInfo *t)
{
  Mask *mask = NULL;

  if (t->spacetype == SPACE_CLIP) {
    SpaceClip *sc = t->sa->spacedata.first;
    mask = ED_space_clip_get_mask(sc);
  }
  else if (t->spacetype == SPACE_IMAGE) {
    SpaceImage *sima = t->sa->spacedata.first;
    mask = ED_space_image_get_mask(sima);
  }
  else {
    BLI_assert(0);
  }

  if (t->scene->nodetree) {
    /* tracks can be used for stabilization nodes,
     * flush update for such nodes */
    // if (nodeUpdateID(t->scene->nodetree, &mask->id))
    {
      WM_event_add_notifier(C, NC_MASK | ND_DATA, &mask->id);
    }
  }

  /* TODO - dont key all masks... */
  if (IS_AUTOKEY_ON(t->scene)) {
    Scene *scene = t->scene;

    ED_mask_layer_shape_auto_key_select(mask, CFRA);
  }
}

static void special_aftertrans_update__node(bContext *C, TransInfo *t)
{
  Main *bmain = CTX_data_main(C);
  const bool canceled = (t->state == TRANS_CANCEL);

  if (canceled && t->remove_on_cancel) {
    /* remove selected nodes on cancel */
    SpaceNode *snode = (SpaceNode *)t->sa->spacedata.first;
    bNodeTree *ntree = snode->edittree;
    if (ntree) {
      bNode *node, *node_next;
      for (node = ntree->nodes.first; node; node = node_next) {
        node_next = node->next;
        if (node->flag & NODE_SELECT) {
          nodeRemoveNode(bmain, ntree, node, true);
        }
      }
    }
  }
}

static void special_aftertrans_update__mesh(bContext *UNUSED(C), TransInfo *t)
{
  /* so automerge supports mirror */
  if ((t->scene->toolsettings->automerge) && ((t->flag & T_EDIT) && t->obedit_type == OB_MESH)) {
    FOREACH_TRANS_DATA_CONTAINER (t, tc) {

      BMEditMesh *em = BKE_editmesh_from_object(tc->obedit);
      BMesh *bm = em->bm;
      char hflag;
      bool has_face_sel = (bm->totfacesel != 0);

      if (tc->mirror.axis_flag) {
        TransData *td;
        int i;

        /* Rather then adjusting the selection (which the user would notice)
         * tag all mirrored verts, then auto-merge those. */
        BM_mesh_elem_hflag_disable_all(bm, BM_VERT, BM_ELEM_TAG, false);

        for (i = 0, td = tc->data; i < tc->data_len; i++, td++) {
          if (td->extra) {
            BM_elem_flag_enable((BMVert *)td->extra, BM_ELEM_TAG);
          }
        }

        hflag = BM_ELEM_SELECT | BM_ELEM_TAG;
      }
      else {
        hflag = BM_ELEM_SELECT;
      }

      if (t->scene->toolsettings->automerge & AUTO_MERGE) {
        if (t->scene->toolsettings->automerge & AUTO_MERGE_AND_SPLIT) {
          EDBM_automerge_and_split(
              tc->obedit, true, true, true, hflag, t->scene->toolsettings->doublimit);
        }
        else {
          EDBM_automerge(tc->obedit, true, hflag, t->scene->toolsettings->doublimit);
        }
      }

      /* Special case, this is needed or faces won't re-select.
       * Flush selected edges to faces. */
      if (has_face_sel && (em->selectmode == SCE_SELECT_FACE)) {
        EDBM_selectmode_flush_ex(em, SCE_SELECT_EDGE);
      }
    }
  }
}

/* inserting keys, pointcache, redraw events... */
/**
 * \note Sequencer freeing has its own function now because of a conflict
 * with transform's order of freeing (campbell).
 * Order changed, the sequencer stuff should go back in here
 */
void special_aftertrans_update(bContext *C, TransInfo *t)
{
  Main *bmain = CTX_data_main(t->context);
  BLI_assert(bmain == CTX_data_main(C));

  Object *ob;
  //  short redrawipo=0, resetslowpar=1;
  const bool canceled = (t->state == TRANS_CANCEL);
  const bool duplicate = (t->mode == TFM_TIME_DUPLICATE);

  /* early out when nothing happened */
  if (t->data_len_all == 0 || t->mode == TFM_DUMMY) {
    return;
  }

  if (t->spacetype == SPACE_VIEW3D) {
    if (t->flag & T_EDIT) {
      /* Special Exception:
       * We don't normally access 't->custom.mode' here, but its needed in this case. */

      if (canceled == 0) {
        /* we need to delete the temporary faces before automerging */
        if (t->mode == TFM_EDGE_SLIDE) {
          /* handle multires re-projection, done
           * on transform completion since it's
           * really slow -joeedh */
          projectEdgeSlideData(t, true);

          FOREACH_TRANS_DATA_CONTAINER (t, tc) {
            EdgeSlideData *sld = tc->custom.mode.data;

            if (sld == NULL) {
              continue;
            }

            /* Free temporary faces to avoid auto-merging and deleting
             * during cleanup - psy-fi. */
            freeEdgeSlideTempFaces(sld);
          }
        }
        else if (t->mode == TFM_VERT_SLIDE) {
          /* as above */
          projectVertSlideData(t, true);
          FOREACH_TRANS_DATA_CONTAINER (t, tc) {
            VertSlideData *sld = tc->custom.mode.data;
            freeVertSlideTempFaces(sld);
          }
        }

        if (t->obedit_type == OB_MESH) {
          special_aftertrans_update__mesh(C, t);
        }
      }
      else {
        if (t->mode == TFM_EDGE_SLIDE) {
          EdgeSlideParams *slp = t->custom.mode.data;
          slp->perc = 0.0;
          projectEdgeSlideData(t, false);
        }
        else if (t->mode == TFM_VERT_SLIDE) {
          EdgeSlideParams *slp = t->custom.mode.data;
          slp->perc = 0.0;
          projectVertSlideData(t, false);
        }
      }
    }
  }

  if (t->options & CTX_GPENCIL_STROKES) {
    /* pass */
  }
  else if (t->spacetype == SPACE_SEQ) {
    /* freeSeqData in transform_conversions.c does this
     * keep here so the else at the end wont run... */

    SpaceSeq *sseq = (SpaceSeq *)t->sa->spacedata.first;

    /* marker transform, not especially nice but we may want to move markers
     * at the same time as keyframes in the dope sheet. */
    if ((sseq->flag & SEQ_MARKER_TRANS) && (canceled == 0)) {
      /* cant use TFM_TIME_EXTEND
       * for some reason EXTEND is changed into TRANSLATE, so use frame_side instead */

      if (t->mode == TFM_SEQ_SLIDE) {
        if (t->frame_side == 'B') {
          ED_markers_post_apply_transform(
              &t->scene->markers, t->scene, TFM_TIME_TRANSLATE, t->values[0], t->frame_side);
        }
      }
      else if (ELEM(t->frame_side, 'L', 'R')) {
        ED_markers_post_apply_transform(
            &t->scene->markers, t->scene, TFM_TIME_EXTEND, t->values[0], t->frame_side);
      }
    }
  }
  else if (t->spacetype == SPACE_IMAGE) {
    if (t->options & CTX_MASK) {
      special_aftertrans_update__mask(C, t);
    }
  }
  else if (t->spacetype == SPACE_NODE) {
    SpaceNode *snode = (SpaceNode *)t->sa->spacedata.first;
    special_aftertrans_update__node(C, t);
    if (canceled == 0) {
      ED_node_post_apply_transform(C, snode->edittree);

      ED_node_link_insert(bmain, t->sa);
    }

    /* clear link line */
    ED_node_link_intersect_test(t->sa, 0);
  }
  else if (t->spacetype == SPACE_CLIP) {
    if (t->options & CTX_MOVIECLIP) {
      special_aftertrans_update__movieclip(C, t);
    }
    else if (t->options & CTX_MASK) {
      special_aftertrans_update__mask(C, t);
    }
  }
  else if (t->spacetype == SPACE_ACTION) {
    SpaceAction *saction = (SpaceAction *)t->sa->spacedata.first;
    bAnimContext ac;

    /* initialize relevant anim-context 'context' data */
    if (ANIM_animdata_get_context(C, &ac) == 0) {
      return;
    }

    ob = ac.obact;

    if (ELEM(ac.datatype, ANIMCONT_DOPESHEET, ANIMCONT_SHAPEKEY, ANIMCONT_TIMELINE)) {
      ListBase anim_data = {NULL, NULL};
      bAnimListElem *ale;
      short filter = (ANIMFILTER_DATA_VISIBLE | ANIMFILTER_FOREDIT /*| ANIMFILTER_CURVESONLY*/);

      /* get channels to work on */
      ANIM_animdata_filter(&ac, &anim_data, filter, ac.data, ac.datatype);

      /* these should all be F-Curves */
      for (ale = anim_data.first; ale; ale = ale->next) {
        AnimData *adt = ANIM_nla_mapping_get(&ac, ale);
        FCurve *fcu = (FCurve *)ale->key_data;

        /* 3 cases here for curve cleanups:
         * 1) NOTRANSKEYCULL on    -> cleanup of duplicates shouldn't be done
         * 2) canceled == 0        -> user confirmed the transform,
         *                            so duplicates should be removed
         * 3) canceled + duplicate -> user canceled the transform,
         *                            but we made duplicates, so get rid of these
         */
        if ((saction->flag & SACTION_NOTRANSKEYCULL) == 0 && ((canceled == 0) || (duplicate))) {
          if (adt) {
            ANIM_nla_mapping_apply_fcurve(adt, fcu, 0, 0);
            posttrans_fcurve_clean(fcu, false); /* only use handles in graph editor */
            ANIM_nla_mapping_apply_fcurve(adt, fcu, 1, 0);
          }
          else {
            posttrans_fcurve_clean(fcu, false); /* only use handles in graph editor */
          }
        }
      }

      /* free temp memory */
      ANIM_animdata_freelist(&anim_data);
    }
    else if (ac.datatype == ANIMCONT_ACTION) {  // TODO: just integrate into the above...
      /* Depending on the lock status, draw necessary views */
      // fixme... some of this stuff is not good
      if (ob) {
        if (ob->pose || BKE_key_from_object(ob)) {
          DEG_id_tag_update(&ob->id,
                            ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY | ID_RECALC_ANIMATION);
        }
        else {
          DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM);
        }
      }

      /* 3 cases here for curve cleanups:
       * 1) NOTRANSKEYCULL on    -> cleanup of duplicates shouldn't be done
       * 2) canceled == 0        -> user confirmed the transform,
       *                            so duplicates should be removed.
       * 3) canceled + duplicate -> user canceled the transform,
       *                            but we made duplicates, so get rid of these.
       */
      if ((saction->flag & SACTION_NOTRANSKEYCULL) == 0 && ((canceled == 0) || (duplicate))) {
        posttrans_action_clean(&ac, (bAction *)ac.data);
      }
    }
    else if (ac.datatype == ANIMCONT_GPENCIL) {
      /* remove duplicate frames and also make sure points are in order! */
      /* 3 cases here for curve cleanups:
       * 1) NOTRANSKEYCULL on    -> cleanup of duplicates shouldn't be done
       * 2) canceled == 0        -> user confirmed the transform,
       *                            so duplicates should be removed
       * 3) canceled + duplicate -> user canceled the transform,
       *                            but we made duplicates, so get rid of these
       */
      if ((saction->flag & SACTION_NOTRANSKEYCULL) == 0 && ((canceled == 0) || (duplicate))) {
        bGPdata *gpd;

        // XXX: BAD! this get gpencil datablocks directly from main db...
        // but that's how this currently works :/
        for (gpd = bmain->gpencils.first; gpd; gpd = gpd->id.next) {
          if (ID_REAL_USERS(gpd)) {
            posttrans_gpd_clean(gpd);
          }
        }
      }
    }
    else if (ac.datatype == ANIMCONT_MASK) {
      /* remove duplicate frames and also make sure points are in order! */
      /* 3 cases here for curve cleanups:
       * 1) NOTRANSKEYCULL on:
       *    Cleanup of duplicates shouldn't be done.
       * 2) canceled == 0:
       *    User confirmed the transform, so duplicates should be removed.
       * 3) Canceled + duplicate:
       *    User canceled the transform, but we made duplicates, so get rid of these.
       */
      if ((saction->flag & SACTION_NOTRANSKEYCULL) == 0 && ((canceled == 0) || (duplicate))) {
        Mask *mask;

        // XXX: BAD! this get gpencil datablocks directly from main db...
        // but that's how this currently works :/
        for (mask = bmain->masks.first; mask; mask = mask->id.next) {
          if (ID_REAL_USERS(mask)) {
            posttrans_mask_clean(mask);
          }
        }
      }
    }

    /* marker transform, not especially nice but we may want to move markers
     * at the same time as keyframes in the dope sheet.
     */
    if ((saction->flag & SACTION_MARKERS_MOVE) && (canceled == 0)) {
      if (t->mode == TFM_TIME_TRANSLATE) {
#if 0
        if (ELEM(t->frame_side, 'L', 'R')) { /* TFM_TIME_EXTEND */
          /* same as below */
          ED_markers_post_apply_transform(
              ED_context_get_markers(C), t->scene, t->mode, t->values[0], t->frame_side);
        }
        else /* TFM_TIME_TRANSLATE */
#endif
        {
          ED_markers_post_apply_transform(
              ED_context_get_markers(C), t->scene, t->mode, t->values[0], t->frame_side);
        }
      }
      else if (t->mode == TFM_TIME_SCALE) {
        ED_markers_post_apply_transform(
            ED_context_get_markers(C), t->scene, t->mode, t->values[0], t->frame_side);
      }
    }

    /* make sure all F-Curves are set correctly */
    if (!ELEM(ac.datatype, ANIMCONT_GPENCIL, ANIMCONT_MASK)) {
      ANIM_editkeyframes_refresh(&ac);
    }

    /* clear flag that was set for time-slide drawing */
    saction->flag &= ~SACTION_MOVING;
  }
  else if (t->spacetype == SPACE_GRAPH) {
    SpaceGraph *sipo = (SpaceGraph *)t->sa->spacedata.first;
    bAnimContext ac;
    const bool use_handle = (sipo->flag & SIPO_NOHANDLES) == 0;

    /* initialize relevant anim-context 'context' data */
    if (ANIM_animdata_get_context(C, &ac) == 0) {
      return;
    }

    if (ac.datatype) {
      ListBase anim_data = {NULL, NULL};
      bAnimListElem *ale;
      short filter = (ANIMFILTER_DATA_VISIBLE | ANIMFILTER_FOREDIT | ANIMFILTER_CURVE_VISIBLE);

      /* get channels to work on */
      ANIM_animdata_filter(&ac, &anim_data, filter, ac.data, ac.datatype);

      for (ale = anim_data.first; ale; ale = ale->next) {
        AnimData *adt = ANIM_nla_mapping_get(&ac, ale);
        FCurve *fcu = (FCurve *)ale->key_data;

        /* 3 cases here for curve cleanups:
         * 1) NOTRANSKEYCULL on    -> cleanup of duplicates shouldn't be done
         * 2) canceled == 0        -> user confirmed the transform,
         *                            so duplicates should be removed
         * 3) canceled + duplicate -> user canceled the transform,
         *                            but we made duplicates, so get rid of these
         */
        if ((sipo->flag & SIPO_NOTRANSKEYCULL) == 0 && ((canceled == 0) || (duplicate))) {
          if (adt) {
            ANIM_nla_mapping_apply_fcurve(adt, fcu, 0, 0);
            posttrans_fcurve_clean(fcu, use_handle);
            ANIM_nla_mapping_apply_fcurve(adt, fcu, 1, 0);
          }
          else {
            posttrans_fcurve_clean(fcu, use_handle);
          }
        }
      }

      /* free temp memory */
      ANIM_animdata_freelist(&anim_data);
    }

    /* Make sure all F-Curves are set correctly, but not if transform was
     * canceled, since then curves were already restored to initial state.
     * Note: if the refresh is really needed after cancel then some way
     *       has to be added to not update handle types (see bug 22289).
     */
    if (!canceled) {
      ANIM_editkeyframes_refresh(&ac);
    }
  }
  else if (t->spacetype == SPACE_NLA) {
    bAnimContext ac;

    /* initialize relevant anim-context 'context' data */
    if (ANIM_animdata_get_context(C, &ac) == 0) {
      return;
    }

    if (ac.datatype) {
      ListBase anim_data = {NULL, NULL};
      bAnimListElem *ale;
      short filter = (ANIMFILTER_DATA_VISIBLE | ANIMFILTER_FOREDIT);

      /* get channels to work on */
      ANIM_animdata_filter(&ac, &anim_data, filter, ac.data, ac.datatype);

      for (ale = anim_data.first; ale; ale = ale->next) {
        NlaTrack *nlt = (NlaTrack *)ale->data;

        /* make sure strips are in order again */
        BKE_nlatrack_sort_strips(nlt);

        /* remove the temp metas */
        BKE_nlastrips_clear_metas(&nlt->strips, 0, 1);
      }

      /* free temp memory */
      ANIM_animdata_freelist(&anim_data);

      /* perform after-transfrom validation */
      ED_nla_postop_refresh(&ac);
    }
  }
  else if (t->flag & T_EDIT) {
    if (t->obedit_type == OB_MESH) {
      FOREACH_TRANS_DATA_CONTAINER (t, tc) {
        BMEditMesh *em = BKE_editmesh_from_object(tc->obedit);
        /* table needs to be created for each edit command, since vertices can move etc */
        ED_mesh_mirror_spatial_table(tc->obedit, em, NULL, NULL, 'e');
        /* TODO(campbell): xform: We need support for many mirror objects at once! */
        break;
      }
    }
  }
  else if (t->flag & T_POSE && (t->mode == TFM_BONESIZE)) {
    /* Handle the exception where for TFM_BONESIZE in edit mode we pretend to be
     * in pose mode (to use bone orientation matrix),
     * in that case we don't do operations like auto-keyframing. */
    FOREACH_TRANS_DATA_CONTAINER (t, tc) {
      ob = tc->poseobj;
      DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
    }
  }
  else if (t->flag & T_POSE) {
    GSet *motionpath_updates = BLI_gset_ptr_new("motionpath updates");

    FOREACH_TRANS_DATA_CONTAINER (t, tc) {

      bPoseChannel *pchan;
      short targetless_ik = 0;

      ob = tc->poseobj;

      if ((t->flag & T_AUTOIK) && (t->options & CTX_AUTOCONFIRM)) {
        /* when running transform non-interactively (operator exec),
         * we need to update the pose otherwise no updates get called during
         * transform and the auto-ik is not applied. see [#26164] */
        struct Object *pose_ob = tc->poseobj;
        BKE_pose_where_is(t->depsgraph, t->scene, pose_ob);
      }

      /* set BONE_TRANSFORM flags for autokey, gizmo draw might have changed them */
      if (!canceled && (t->mode != TFM_DUMMY)) {
        count_set_pose_transflags(ob, t->mode, t->around, NULL);
      }

      /* if target-less IK grabbing, we calculate the pchan transforms and clear flag */
      if (!canceled && t->mode == TFM_TRANSLATION) {
        targetless_ik = apply_targetless_ik(ob);
      }
      else {
        /* not forget to clear the auto flag */
        for (pchan = ob->pose->chanbase.first; pchan; pchan = pchan->next) {
          bKinematicConstraint *data = has_targetless_ik(pchan);
          if (data) {
            data->flag &= ~CONSTRAINT_IK_AUTO;
          }
        }
      }

      if (t->mode == TFM_TRANSLATION) {
        pose_grab_with_ik_clear(bmain, ob);
      }

      /* automatic inserting of keys and unkeyed tagging -
       * only if transform wasn't canceled (or TFM_DUMMY) */
      if (!canceled && (t->mode != TFM_DUMMY)) {
        autokeyframe_pose(C, t->scene, ob, t->mode, targetless_ik);
        DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
      }
      else {
        DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
      }

      if (t->mode != TFM_DUMMY && motionpath_need_update_pose(t->scene, ob)) {
        BLI_gset_insert(motionpath_updates, ob);
      }
    }

    /* Update motion paths once for all transformed bones in an object. */
    GSetIterator gs_iter;
    GSET_ITER (gs_iter, motionpath_updates) {
      bool current_frame_only = canceled;
      ob = BLI_gsetIterator_getKey(&gs_iter);
      ED_pose_recalculate_paths(C, t->scene, ob, current_frame_only);
    }
    BLI_gset_free(motionpath_updates, NULL);
  }
  else if (t->options & CTX_PAINT_CURVE) {
    /* pass */
  }
  else if ((t->view_layer->basact) && (ob = t->view_layer->basact->object) &&
           (ob->mode & OB_MODE_PARTICLE_EDIT) && PE_get_current(t->scene, ob)) {
    /* do nothing */
  }
  else if (t->flag & T_CURSOR) {
    /* do nothing */
  }
  else { /* Objects */
    BLI_assert(t->flag & (T_OBJECT | T_TEXTURE));

    TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);
    bool motionpath_update = false;

    for (int i = 0; i < tc->data_len; i++) {
      TransData *td = tc->data + i;
      ListBase pidlist;
      PTCacheID *pid;
      ob = td->ob;

      if (td->flag & TD_NOACTION) {
        break;
      }

      if (td->flag & TD_SKIP) {
        continue;
      }

      /* flag object caches as outdated */
      BKE_ptcache_ids_from_object(&pidlist, ob, t->scene, MAX_DUPLI_RECUR);
      for (pid = pidlist.first; pid; pid = pid->next) {
        if (pid->type != PTCACHE_TYPE_PARTICLES) {
          /* particles don't need reset on geometry change */
          pid->cache->flag |= PTCACHE_OUTDATED;
        }
      }
      BLI_freelistN(&pidlist);

      /* pointcache refresh */
      if (BKE_ptcache_object_reset(t->scene, ob, PTCACHE_RESET_OUTDATED)) {
        DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
      }

      /* Needed for proper updating of "quick cached" dynamics. */
      /* Creates troubles for moving animated objects without */
      /* autokey though, probably needed is an anim sys override? */
      /* Please remove if some other solution is found. -jahka */
      DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM);

      /* Set autokey if necessary */
      if (!canceled) {
        autokeyframe_object(C, t->scene, t->view_layer, ob, t->mode);
      }

      motionpath_update |= motionpath_need_update_object(t->scene, ob);

      /* restore rigid body transform */
      if (ob->rigidbody_object && canceled) {
        float ctime = BKE_scene_frame_get(t->scene);
        if (BKE_rigidbody_check_sim_running(t->scene->rigidbody_world, ctime)) {
          BKE_rigidbody_aftertrans_update(ob,
                                          td->ext->oloc,
                                          td->ext->orot,
                                          td->ext->oquat,
                                          td->ext->orotAxis,
                                          td->ext->orotAngle);
        }
      }
    }

    if (motionpath_update) {
      /* Update motion paths once for all transformed objects. */
      bool current_frame_only = canceled;
      ED_objects_recalculate_paths(C, t->scene, current_frame_only);
    }
  }

  clear_trans_object_base_flags(t);
}

int special_transform_moving(TransInfo *t)
{
  if (t->spacetype == SPACE_SEQ) {
    return G_TRANSFORM_SEQ;
  }
  else if (t->spacetype == SPACE_GRAPH) {
    return G_TRANSFORM_FCURVES;
  }
  else if ((t->flag & T_EDIT) || (t->flag & T_POSE)) {
    return G_TRANSFORM_EDIT;
  }
  else if (t->flag & (T_OBJECT | T_TEXTURE)) {
    return G_TRANSFORM_OBJ;
  }

  return 0;
}

/** \} */

/* *** CLIP EDITOR *** */

/* * motion tracking * */

enum transDataTracking_Mode {
  transDataTracking_ModeTracks = 0,
  transDataTracking_ModeCurves = 1,
  transDataTracking_ModePlaneTracks = 2,
};

typedef struct TransDataTracking {
  int mode, flag;

  /* tracks transformation from main window */
  int area;
  const float *relative, *loc;
  float soffset[2], srelative[2];
  float offset[2];

  float (*smarkers)[2];
  int markersnr;
  MovieTrackingMarker *markers;

  /* marker transformation from curves editor */
  float *prev_pos, scale;
  short coord;

  MovieTrackingTrack *track;
  MovieTrackingPlaneTrack *plane_track;
} TransDataTracking;

static void markerToTransDataInit(TransData *td,
                                  TransData2D *td2d,
                                  TransDataTracking *tdt,
                                  MovieTrackingTrack *track,
                                  MovieTrackingMarker *marker,
                                  int area,
                                  float loc[2],
                                  float rel[2],
                                  const float off[2],
                                  const float aspect[2])
{
  int anchor = area == TRACK_AREA_POINT && off;

  tdt->mode = transDataTracking_ModeTracks;

  if (anchor) {
    td2d->loc[0] = rel[0] * aspect[0]; /* hold original location */
    td2d->loc[1] = rel[1] * aspect[1];

    tdt->loc = loc;
    td2d->loc2d = loc; /* current location */
  }
  else {
    td2d->loc[0] = loc[0] * aspect[0]; /* hold original location */
    td2d->loc[1] = loc[1] * aspect[1];

    td2d->loc2d = loc; /* current location */
  }
  td2d->loc[2] = 0.0f;

  tdt->relative = rel;
  tdt->area = area;

  tdt->markersnr = track->markersnr;
  tdt->markers = track->markers;
  tdt->track = track;

  if (rel) {
    if (!anchor) {
      td2d->loc[0] += rel[0] * aspect[0];
      td2d->loc[1] += rel[1] * aspect[1];
    }

    copy_v2_v2(tdt->srelative, rel);
  }

  if (off) {
    copy_v2_v2(tdt->soffset, off);
  }

  td->flag = 0;
  td->loc = td2d->loc;
  copy_v3_v3(td->iloc, td->loc);

  // copy_v3_v3(td->center, td->loc);
  td->flag |= TD_INDIVIDUAL_SCALE;
  td->center[0] = marker->pos[0] * aspect[0];
  td->center[1] = marker->pos[1] * aspect[1];

  memset(td->axismtx, 0, sizeof(td->axismtx));
  td->axismtx[2][2] = 1.0f;

  td->ext = NULL;
  td->val = NULL;

  td->flag |= TD_SELECTED;
  td->dist = 0.0;

  unit_m3(td->mtx);
  unit_m3(td->smtx);
}

static void trackToTransData(const int framenr,
                             TransData *td,
                             TransData2D *td2d,
                             TransDataTracking *tdt,
                             MovieTrackingTrack *track,
                             const float aspect[2])
{
  MovieTrackingMarker *marker = BKE_tracking_marker_ensure(track, framenr);

  tdt->flag = marker->flag;
  marker->flag &= ~(MARKER_DISABLED | MARKER_TRACKED);

  markerToTransDataInit(td++,
                        td2d++,
                        tdt++,
                        track,
                        marker,
                        TRACK_AREA_POINT,
                        track->offset,
                        marker->pos,
                        track->offset,
                        aspect);

  if (track->flag & SELECT) {
    markerToTransDataInit(
        td++, td2d++, tdt++, track, marker, TRACK_AREA_POINT, marker->pos, NULL, NULL, aspect);
  }

  if (track->pat_flag & SELECT) {
    int a;

    for (a = 0; a < 4; a++) {
      markerToTransDataInit(td++,
                            td2d++,
                            tdt++,
                            track,
                            marker,
                            TRACK_AREA_PAT,
                            marker->pattern_corners[a],
                            marker->pos,
                            NULL,
                            aspect);
    }
  }

  if (track->search_flag & SELECT) {
    markerToTransDataInit(td++,
                          td2d++,
                          tdt++,
                          track,
                          marker,
                          TRACK_AREA_SEARCH,
                          marker->search_min,
                          marker->pos,
                          NULL,
                          aspect);

    markerToTransDataInit(td++,
                          td2d++,
                          tdt++,
                          track,
                          marker,
                          TRACK_AREA_SEARCH,
                          marker->search_max,
                          marker->pos,
                          NULL,
                          aspect);
  }
}

static void planeMarkerToTransDataInit(TransData *td,
                                       TransData2D *td2d,
                                       TransDataTracking *tdt,
                                       MovieTrackingPlaneTrack *plane_track,
                                       float corner[2],
                                       const float aspect[2])
{
  tdt->mode = transDataTracking_ModePlaneTracks;
  tdt->plane_track = plane_track;

  td2d->loc[0] = corner[0] * aspect[0]; /* hold original location */
  td2d->loc[1] = corner[1] * aspect[1];

  td2d->loc2d = corner; /* current location */
  td2d->loc[2] = 0.0f;

  td->flag = 0;
  td->loc = td2d->loc;
  copy_v3_v3(td->iloc, td->loc);
  copy_v3_v3(td->center, td->loc);

  memset(td->axismtx, 0, sizeof(td->axismtx));
  td->axismtx[2][2] = 1.0f;

  td->ext = NULL;
  td->val = NULL;

  td->flag |= TD_SELECTED;
  td->dist = 0.0;

  unit_m3(td->mtx);
  unit_m3(td->smtx);
}

static void planeTrackToTransData(const int framenr,
                                  TransData *td,
                                  TransData2D *td2d,
                                  TransDataTracking *tdt,
                                  MovieTrackingPlaneTrack *plane_track,
                                  const float aspect[2])
{
  MovieTrackingPlaneMarker *plane_marker = BKE_tracking_plane_marker_ensure(plane_track, framenr);
  int i;

  tdt->flag = plane_marker->flag;
  plane_marker->flag &= ~PLANE_MARKER_TRACKED;

  for (i = 0; i < 4; i++) {
    planeMarkerToTransDataInit(td++, td2d++, tdt++, plane_track, plane_marker->corners[i], aspect);
  }
}

static void transDataTrackingFree(TransInfo *UNUSED(t),
                                  TransDataContainer *UNUSED(tc),
                                  TransCustomData *custom_data)
{
  if (custom_data->data) {
    TransDataTracking *tdt = custom_data->data;
    if (tdt->smarkers) {
      MEM_freeN(tdt->smarkers);
    }

    MEM_freeN(tdt);
    custom_data->data = NULL;
  }
}

static void createTransTrackingTracksData(bContext *C, TransInfo *t)
{
  TransData *td;
  TransData2D *td2d;
  SpaceClip *sc = CTX_wm_space_clip(C);
  MovieClip *clip = ED_space_clip_get_clip(sc);
  ListBase *tracksbase = BKE_tracking_get_active_tracks(&clip->tracking);
  ListBase *plane_tracks_base = BKE_tracking_get_active_plane_tracks(&clip->tracking);
  MovieTrackingTrack *track;
  MovieTrackingPlaneTrack *plane_track;
  TransDataTracking *tdt;
  int framenr = ED_space_clip_get_clip_frame_number(sc);

  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);

  /* count */
  tc->data_len = 0;

  track = tracksbase->first;
  while (track) {
    if (TRACK_VIEW_SELECTED(sc, track) && (track->flag & TRACK_LOCKED) == 0) {
      tc->data_len++; /* offset */

      if (track->flag & SELECT) {
        tc->data_len++;
      }

      if (track->pat_flag & SELECT) {
        tc->data_len += 4;
      }

      if (track->search_flag & SELECT) {
        tc->data_len += 2;
      }
    }

    track = track->next;
  }

  for (plane_track = plane_tracks_base->first; plane_track; plane_track = plane_track->next) {
    if (PLANE_TRACK_VIEW_SELECTED(plane_track)) {
      tc->data_len += 4;
    }
  }

  if (tc->data_len == 0) {
    return;
  }

  td = tc->data = MEM_callocN(tc->data_len * sizeof(TransData), "TransTracking TransData");
  td2d = tc->data_2d = MEM_callocN(tc->data_len * sizeof(TransData2D),
                                   "TransTracking TransData2D");
  tdt = tc->custom.type.data = MEM_callocN(tc->data_len * sizeof(TransDataTracking),
                                           "TransTracking TransDataTracking");

  tc->custom.type.free_cb = transDataTrackingFree;

  /* create actual data */
  track = tracksbase->first;
  while (track) {
    if (TRACK_VIEW_SELECTED(sc, track) && (track->flag & TRACK_LOCKED) == 0) {
      trackToTransData(framenr, td, td2d, tdt, track, t->aspect);

      /* offset */
      td++;
      td2d++;
      tdt++;

      if (track->flag & SELECT) {
        td++;
        td2d++;
        tdt++;
      }

      if (track->pat_flag & SELECT) {
        td += 4;
        td2d += 4;
        tdt += 4;
      }

      if (track->search_flag & SELECT) {
        td += 2;
        td2d += 2;
        tdt += 2;
      }
    }

    track = track->next;
  }

  for (plane_track = plane_tracks_base->first; plane_track; plane_track = plane_track->next) {
    if (PLANE_TRACK_VIEW_SELECTED(plane_track)) {
      planeTrackToTransData(framenr, td, td2d, tdt, plane_track, t->aspect);
      td += 4;
      td2d += 4;
      tdt += 4;
    }
  }
}

static void markerToTransCurveDataInit(TransData *td,
                                       TransData2D *td2d,
                                       TransDataTracking *tdt,
                                       MovieTrackingTrack *track,
                                       MovieTrackingMarker *marker,
                                       MovieTrackingMarker *prev_marker,
                                       short coord,
                                       float size)
{
  float frames_delta = (marker->framenr - prev_marker->framenr);

  tdt->flag = marker->flag;
  marker->flag &= ~MARKER_TRACKED;

  tdt->mode = transDataTracking_ModeCurves;
  tdt->coord = coord;
  tdt->scale = 1.0f / size * frames_delta;
  tdt->prev_pos = prev_marker->pos;
  tdt->track = track;

  /* calculate values depending on marker's speed */
  td2d->loc[0] = marker->framenr;
  td2d->loc[1] = (marker->pos[coord] - prev_marker->pos[coord]) * size / frames_delta;
  td2d->loc[2] = 0.0f;

  td2d->loc2d = marker->pos; /* current location */

  td->flag = 0;
  td->loc = td2d->loc;
  copy_v3_v3(td->center, td->loc);
  copy_v3_v3(td->iloc, td->loc);

  memset(td->axismtx, 0, sizeof(td->axismtx));
  td->axismtx[2][2] = 1.0f;

  td->ext = NULL;
  td->val = NULL;

  td->flag |= TD_SELECTED;
  td->dist = 0.0;

  unit_m3(td->mtx);
  unit_m3(td->smtx);
}

static void createTransTrackingCurvesData(bContext *C, TransInfo *t)
{
  TransData *td;
  TransData2D *td2d;
  SpaceClip *sc = CTX_wm_space_clip(C);
  MovieClip *clip = ED_space_clip_get_clip(sc);
  ListBase *tracksbase = BKE_tracking_get_active_tracks(&clip->tracking);
  MovieTrackingTrack *track;
  MovieTrackingMarker *marker, *prev_marker;
  TransDataTracking *tdt;
  int i, width, height;

  BKE_movieclip_get_size(clip, &sc->user, &width, &height);

  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);

  /* count */
  tc->data_len = 0;

  if ((sc->flag & SC_SHOW_GRAPH_TRACKS_MOTION) == 0) {
    return;
  }

  track = tracksbase->first;
  while (track) {
    if (TRACK_VIEW_SELECTED(sc, track) && (track->flag & TRACK_LOCKED) == 0) {
      for (i = 1; i < track->markersnr; i++) {
        marker = &track->markers[i];
        prev_marker = &track->markers[i - 1];

        if ((marker->flag & MARKER_DISABLED) || (prev_marker->flag & MARKER_DISABLED)) {
          continue;
        }

        if (marker->flag & MARKER_GRAPH_SEL_X) {
          tc->data_len += 1;
        }

        if (marker->flag & MARKER_GRAPH_SEL_Y) {
          tc->data_len += 1;
        }
      }
    }

    track = track->next;
  }

  if (tc->data_len == 0) {
    return;
  }

  td = tc->data = MEM_callocN(tc->data_len * sizeof(TransData), "TransTracking TransData");
  td2d = tc->data_2d = MEM_callocN(tc->data_len * sizeof(TransData2D),
                                   "TransTracking TransData2D");
  tc->custom.type.data = tdt = MEM_callocN(tc->data_len * sizeof(TransDataTracking),
                                           "TransTracking TransDataTracking");
  tc->custom.type.free_cb = transDataTrackingFree;

  /* create actual data */
  track = tracksbase->first;
  while (track) {
    if (TRACK_VIEW_SELECTED(sc, track) && (track->flag & TRACK_LOCKED) == 0) {
      for (i = 1; i < track->markersnr; i++) {
        marker = &track->markers[i];
        prev_marker = &track->markers[i - 1];

        if ((marker->flag & MARKER_DISABLED) || (prev_marker->flag & MARKER_DISABLED)) {
          continue;
        }

        if (marker->flag & MARKER_GRAPH_SEL_X) {
          markerToTransCurveDataInit(
              td, td2d, tdt, track, marker, &track->markers[i - 1], 0, width);
          td += 1;
          td2d += 1;
          tdt += 1;
        }

        if (marker->flag & MARKER_GRAPH_SEL_Y) {
          markerToTransCurveDataInit(
              td, td2d, tdt, track, marker, &track->markers[i - 1], 1, height);

          td += 1;
          td2d += 1;
          tdt += 1;
        }
      }
    }

    track = track->next;
  }
}

static void createTransTrackingData(bContext *C, TransInfo *t)
{
  ARegion *ar = CTX_wm_region(C);
  SpaceClip *sc = CTX_wm_space_clip(C);
  MovieClip *clip = ED_space_clip_get_clip(sc);
  int width, height;

  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);

  tc->data_len = 0;

  if (!clip) {
    return;
  }

  BKE_movieclip_get_size(clip, &sc->user, &width, &height);

  if (width == 0 || height == 0) {
    return;
  }

  if (ar->regiontype == RGN_TYPE_PREVIEW) {
    /* transformation was called from graph editor */
    createTransTrackingCurvesData(C, t);
  }
  else {
    createTransTrackingTracksData(C, t);
  }
}

static void cancelTransTracking(TransInfo *t)
{
  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);
  SpaceClip *sc = t->sa->spacedata.first;
  int i, framenr = ED_space_clip_get_clip_frame_number(sc);
  TransDataTracking *tdt_array = tc->custom.type.data;

  i = 0;
  while (i < tc->data_len) {
    TransDataTracking *tdt = &tdt_array[i];

    if (tdt->mode == transDataTracking_ModeTracks) {
      MovieTrackingTrack *track = tdt->track;
      MovieTrackingMarker *marker = BKE_tracking_marker_get(track, framenr);

      marker->flag = tdt->flag;

      if (track->flag & SELECT) {
        i++;
      }

      if (track->pat_flag & SELECT) {
        i += 4;
      }

      if (track->search_flag & SELECT) {
        i += 2;
      }
    }
    else if (tdt->mode == transDataTracking_ModeCurves) {
      MovieTrackingTrack *track = tdt->track;
      MovieTrackingMarker *marker, *prev_marker;
      int a;

      for (a = 1; a < track->markersnr; a++) {
        marker = &track->markers[a];
        prev_marker = &track->markers[a - 1];

        if ((marker->flag & MARKER_DISABLED) || (prev_marker->flag & MARKER_DISABLED)) {
          continue;
        }

        if (marker->flag & (MARKER_GRAPH_SEL_X | MARKER_GRAPH_SEL_Y)) {
          marker->flag = tdt->flag;
        }
      }
    }
    else if (tdt->mode == transDataTracking_ModePlaneTracks) {
      MovieTrackingPlaneTrack *plane_track = tdt->plane_track;
      MovieTrackingPlaneMarker *plane_marker = BKE_tracking_plane_marker_get(plane_track, framenr);

      plane_marker->flag = tdt->flag;
      i += 3;
    }

    i++;
  }
}

void flushTransTracking(TransInfo *t)
{
  TransData *td;
  TransData2D *td2d;
  TransDataTracking *tdt;
  int a;

  if (t->state == TRANS_CANCEL) {
    cancelTransTracking(t);
  }

  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);

  /* flush to 2d vector from internally used 3d vector */
  for (a = 0, td = tc->data, td2d = tc->data_2d, tdt = tc->custom.type.data; a < tc->data_len;
       a++, td2d++, td++, tdt++) {
    if (tdt->mode == transDataTracking_ModeTracks) {
      float loc2d[2];

      if (t->mode == TFM_ROTATION && tdt->area == TRACK_AREA_SEARCH) {
        continue;
      }

      loc2d[0] = td2d->loc[0] / t->aspect[0];
      loc2d[1] = td2d->loc[1] / t->aspect[1];

      if (t->flag & T_ALT_TRANSFORM) {
        if (t->mode == TFM_RESIZE) {
          if (tdt->area != TRACK_AREA_PAT) {
            continue;
          }
        }
        else if (t->mode == TFM_TRANSLATION) {
          if (tdt->area == TRACK_AREA_POINT && tdt->relative) {
            float d[2], d2[2];

            if (!tdt->smarkers) {
              tdt->smarkers = MEM_callocN(sizeof(*tdt->smarkers) * tdt->markersnr,
                                          "flushTransTracking markers");
              for (a = 0; a < tdt->markersnr; a++) {
                copy_v2_v2(tdt->smarkers[a], tdt->markers[a].pos);
              }
            }

            sub_v2_v2v2(d, loc2d, tdt->soffset);
            sub_v2_v2(d, tdt->srelative);

            sub_v2_v2v2(d2, loc2d, tdt->srelative);

            for (a = 0; a < tdt->markersnr; a++) {
              add_v2_v2v2(tdt->markers[a].pos, tdt->smarkers[a], d2);
            }

            negate_v2_v2(td2d->loc2d, d);
          }
        }
      }

      if (tdt->area != TRACK_AREA_POINT || tdt->relative == NULL) {
        td2d->loc2d[0] = loc2d[0];
        td2d->loc2d[1] = loc2d[1];

        if (tdt->relative) {
          sub_v2_v2(td2d->loc2d, tdt->relative);
        }
      }
    }
    else if (tdt->mode == transDataTracking_ModeCurves) {
      td2d->loc2d[tdt->coord] = tdt->prev_pos[tdt->coord] + td2d->loc[1] * tdt->scale;
    }
    else if (tdt->mode == transDataTracking_ModePlaneTracks) {
      td2d->loc2d[0] = td2d->loc[0] / t->aspect[0];
      td2d->loc2d[1] = td2d->loc[1] / t->aspect[1];
    }
  }
}

/* * masking * */

typedef struct TransDataMasking {
  bool is_handle;

  float handle[2], orig_handle[2];
  float vec[3][3];
  MaskSplinePoint *point;
  float parent_matrix[3][3];
  float parent_inverse_matrix[3][3];
  char orig_handle_type;

  eMaskWhichHandle which_handle;
} TransDataMasking;

static void MaskHandleToTransData(MaskSplinePoint *point,
                                  eMaskWhichHandle which_handle,
                                  TransData *td,
                                  TransData2D *td2d,
                                  TransDataMasking *tdm,
                                  const float asp[2],
                                  /*const*/ float parent_matrix[3][3],
                                  /*const*/ float parent_inverse_matrix[3][3])
{
  BezTriple *bezt = &point->bezt;
  const bool is_sel_any = MASKPOINT_ISSEL_ANY(point);

  tdm->point = point;
  copy_m3_m3(tdm->vec, bezt->vec);

  tdm->is_handle = true;
  copy_m3_m3(tdm->parent_matrix, parent_matrix);
  copy_m3_m3(tdm->parent_inverse_matrix, parent_inverse_matrix);

  BKE_mask_point_handle(point, which_handle, tdm->handle);
  tdm->which_handle = which_handle;

  copy_v2_v2(tdm->orig_handle, tdm->handle);

  mul_v2_m3v2(td2d->loc, parent_matrix, tdm->handle);
  td2d->loc[0] *= asp[0];
  td2d->loc[1] *= asp[1];
  td2d->loc[2] = 0.0f;

  td2d->loc2d = tdm->handle;

  td->flag = 0;
  td->loc = td2d->loc;
  mul_v2_m3v2(td->center, parent_matrix, bezt->vec[1]);
  td->center[0] *= asp[0];
  td->center[1] *= asp[1];
  copy_v3_v3(td->iloc, td->loc);

  memset(td->axismtx, 0, sizeof(td->axismtx));
  td->axismtx[2][2] = 1.0f;

  td->ext = NULL;
  td->val = NULL;

  if (is_sel_any) {
    td->flag |= TD_SELECTED;
  }

  td->dist = 0.0;

  unit_m3(td->mtx);
  unit_m3(td->smtx);

  if (which_handle == MASK_WHICH_HANDLE_LEFT) {
    tdm->orig_handle_type = bezt->h1;
  }
  else if (which_handle == MASK_WHICH_HANDLE_RIGHT) {
    tdm->orig_handle_type = bezt->h2;
  }
}

static void MaskPointToTransData(Scene *scene,
                                 MaskSplinePoint *point,
                                 TransData *td,
                                 TransData2D *td2d,
                                 TransDataMasking *tdm,
                                 const bool is_prop_edit,
                                 const float asp[2])
{
  BezTriple *bezt = &point->bezt;
  const bool is_sel_point = MASKPOINT_ISSEL_KNOT(point);
  const bool is_sel_any = MASKPOINT_ISSEL_ANY(point);
  float parent_matrix[3][3], parent_inverse_matrix[3][3];

  BKE_mask_point_parent_matrix_get(point, CFRA, parent_matrix);
  invert_m3_m3(parent_inverse_matrix, parent_matrix);

  if (is_prop_edit || is_sel_point) {
    int i;

    tdm->point = point;
    copy_m3_m3(tdm->vec, bezt->vec);

    for (i = 0; i < 3; i++) {
      copy_m3_m3(tdm->parent_matrix, parent_matrix);
      copy_m3_m3(tdm->parent_inverse_matrix, parent_inverse_matrix);

      /* CV coords are scaled by aspects. this is needed for rotations and
       * proportional editing to be consistent with the stretched CV coords
       * that are displayed. this also means that for display and numinput,
       * and when the CV coords are flushed, these are converted each time */
      mul_v2_m3v2(td2d->loc, parent_matrix, bezt->vec[i]);
      td2d->loc[0] *= asp[0];
      td2d->loc[1] *= asp[1];
      td2d->loc[2] = 0.0f;

      td2d->loc2d = bezt->vec[i];

      td->flag = 0;
      td->loc = td2d->loc;
      mul_v2_m3v2(td->center, parent_matrix, bezt->vec[1]);
      td->center[0] *= asp[0];
      td->center[1] *= asp[1];
      copy_v3_v3(td->iloc, td->loc);

      memset(td->axismtx, 0, sizeof(td->axismtx));
      td->axismtx[2][2] = 1.0f;

      td->ext = NULL;

      if (i == 1) {
        /* scaling weights */
        td->val = &bezt->weight;
        td->ival = *td->val;
      }
      else {
        td->val = NULL;
      }

      if (is_sel_any) {
        td->flag |= TD_SELECTED;
      }
      td->dist = 0.0;

      unit_m3(td->mtx);
      unit_m3(td->smtx);

      if (i == 0) {
        tdm->orig_handle_type = bezt->h1;
      }
      else if (i == 2) {
        tdm->orig_handle_type = bezt->h2;
      }

      td++;
      td2d++;
      tdm++;
    }
  }
  else {
    if (BKE_mask_point_handles_mode_get(point) == MASK_HANDLE_MODE_STICK) {
      MaskHandleToTransData(point,
                            MASK_WHICH_HANDLE_STICK,
                            td,
                            td2d,
                            tdm,
                            asp,
                            parent_matrix,
                            parent_inverse_matrix);

      td++;
      td2d++;
      tdm++;
    }
    else {
      if (bezt->f1 & SELECT) {
        MaskHandleToTransData(point,
                              MASK_WHICH_HANDLE_LEFT,
                              td,
                              td2d,
                              tdm,
                              asp,
                              parent_matrix,
                              parent_inverse_matrix);

        if (bezt->h1 == HD_VECT) {
          bezt->h1 = HD_FREE;
        }
        else if (bezt->h1 == HD_AUTO) {
          bezt->h1 = HD_ALIGN_DOUBLESIDE;
          bezt->h2 = HD_ALIGN_DOUBLESIDE;
        }

        td++;
        td2d++;
        tdm++;
      }
      if (bezt->f3 & SELECT) {
        MaskHandleToTransData(point,
                              MASK_WHICH_HANDLE_RIGHT,
                              td,
                              td2d,
                              tdm,
                              asp,
                              parent_matrix,
                              parent_inverse_matrix);

        if (bezt->h2 == HD_VECT) {
          bezt->h2 = HD_FREE;
        }
        else if (bezt->h2 == HD_AUTO) {
          bezt->h1 = HD_ALIGN_DOUBLESIDE;
          bezt->h2 = HD_ALIGN_DOUBLESIDE;
        }

        td++;
        td2d++;
        tdm++;
      }
    }
  }
}

static void createTransMaskingData(bContext *C, TransInfo *t)
{
  Scene *scene = CTX_data_scene(C);
  Mask *mask = CTX_data_edit_mask(C);
  MaskLayer *masklay;
  TransData *td = NULL;
  TransData2D *td2d = NULL;
  TransDataMasking *tdm = NULL;
  int count = 0, countsel = 0;
  const bool is_prop_edit = (t->flag & T_PROP_EDIT);
  float asp[2];

  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);

  tc->data_len = 0;

  if (!mask) {
    return;
  }

  if (t->spacetype == SPACE_CLIP) {
    SpaceClip *sc = t->sa->spacedata.first;
    MovieClip *clip = ED_space_clip_get_clip(sc);
    if (!clip) {
      return;
    }
  }

  /* count */
  for (masklay = mask->masklayers.first; masklay; masklay = masklay->next) {
    MaskSpline *spline;

    if (masklay->restrictflag & (MASK_RESTRICT_VIEW | MASK_RESTRICT_SELECT)) {
      continue;
    }

    for (spline = masklay->splines.first; spline; spline = spline->next) {
      int i;

      for (i = 0; i < spline->tot_point; i++) {
        MaskSplinePoint *point = &spline->points[i];

        if (MASKPOINT_ISSEL_ANY(point)) {
          if (MASKPOINT_ISSEL_KNOT(point)) {
            countsel += 3;
          }
          else {
            if (BKE_mask_point_handles_mode_get(point) == MASK_HANDLE_MODE_STICK) {
              countsel += 1;
            }
            else {
              BezTriple *bezt = &point->bezt;
              if (bezt->f1 & SELECT) {
                countsel++;
              }
              if (bezt->f3 & SELECT) {
                countsel++;
              }
            }
          }
        }

        if (is_prop_edit) {
          count += 3;
        }
      }
    }
  }

  /* note: in prop mode we need at least 1 selected */
  if (countsel == 0) {
    return;
  }

  ED_mask_get_aspect(t->sa, t->ar, &asp[0], &asp[1]);

  tc->data_len = (is_prop_edit) ? count : countsel;
  td = tc->data = MEM_callocN(tc->data_len * sizeof(TransData), "TransObData(Mask Editing)");
  /* for each 2d uv coord a 3d vector is allocated, so that they can be
   * treated just as if they were 3d verts */
  td2d = tc->data_2d = MEM_callocN(tc->data_len * sizeof(TransData2D),
                                   "TransObData2D(Mask Editing)");
  tc->custom.type.data = tdm = MEM_callocN(tc->data_len * sizeof(TransDataMasking),
                                           "TransDataMasking(Mask Editing)");
  tc->custom.type.use_free = true;

  /* create data */
  for (masklay = mask->masklayers.first; masklay; masklay = masklay->next) {
    MaskSpline *spline;

    if (masklay->restrictflag & (MASK_RESTRICT_VIEW | MASK_RESTRICT_SELECT)) {
      continue;
    }

    for (spline = masklay->splines.first; spline; spline = spline->next) {
      int i;

      for (i = 0; i < spline->tot_point; i++) {
        MaskSplinePoint *point = &spline->points[i];

        if (is_prop_edit || MASKPOINT_ISSEL_ANY(point)) {
          MaskPointToTransData(scene, point, td, td2d, tdm, is_prop_edit, asp);

          if (is_prop_edit || MASKPOINT_ISSEL_KNOT(point)) {
            td += 3;
            td2d += 3;
            tdm += 3;
          }
          else {
            if (BKE_mask_point_handles_mode_get(point) == MASK_HANDLE_MODE_STICK) {
              td++;
              td2d++;
              tdm++;
            }
            else {
              BezTriple *bezt = &point->bezt;
              if (bezt->f1 & SELECT) {
                td++;
                td2d++;
                tdm++;
              }
              if (bezt->f3 & SELECT) {
                td++;
                td2d++;
                tdm++;
              }
            }
          }
        }
      }
    }
  }
}

void flushTransMasking(TransInfo *t)
{
  TransData2D *td;
  TransDataMasking *tdm;
  int a;
  float asp[2], inv[2];

  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);

  ED_mask_get_aspect(t->sa, t->ar, &asp[0], &asp[1]);
  inv[0] = 1.0f / asp[0];
  inv[1] = 1.0f / asp[1];

  /* flush to 2d vector from internally used 3d vector */
  for (a = 0, td = tc->data_2d, tdm = tc->custom.type.data; a < tc->data_len; a++, td++, tdm++) {
    td->loc2d[0] = td->loc[0] * inv[0];
    td->loc2d[1] = td->loc[1] * inv[1];
    mul_m3_v2(tdm->parent_inverse_matrix, td->loc2d);

    if (tdm->is_handle) {
      BKE_mask_point_set_handle(tdm->point,
                                tdm->which_handle,
                                td->loc2d,
                                (t->flag & T_ALT_TRANSFORM) != 0,
                                tdm->orig_handle,
                                tdm->vec);
    }

    if (t->state == TRANS_CANCEL) {
      if (tdm->which_handle == MASK_WHICH_HANDLE_LEFT) {
        tdm->point->bezt.h1 = tdm->orig_handle_type;
      }
      else if (tdm->which_handle == MASK_WHICH_HANDLE_RIGHT) {
        tdm->point->bezt.h2 = tdm->orig_handle_type;
      }
    }
  }
}

typedef struct TransDataPaintCurve {
  PaintCurvePoint *pcp; /* initial curve point */
  char id;
} TransDataPaintCurve;

#define PC_IS_ANY_SEL(pc) (((pc)->bez.f1 | (pc)->bez.f2 | (pc)->bez.f3) & SELECT)

static void PaintCurveConvertHandle(
    PaintCurvePoint *pcp, int id, TransData2D *td2d, TransDataPaintCurve *tdpc, TransData *td)
{
  BezTriple *bezt = &pcp->bez;
  copy_v2_v2(td2d->loc, bezt->vec[id]);
  td2d->loc[2] = 0.0f;
  td2d->loc2d = bezt->vec[id];

  td->flag = 0;
  td->loc = td2d->loc;
  copy_v3_v3(td->center, bezt->vec[1]);
  copy_v3_v3(td->iloc, td->loc);

  memset(td->axismtx, 0, sizeof(td->axismtx));
  td->axismtx[2][2] = 1.0f;

  td->ext = NULL;
  td->val = NULL;
  td->flag |= TD_SELECTED;
  td->dist = 0.0;

  unit_m3(td->mtx);
  unit_m3(td->smtx);

  tdpc->id = id;
  tdpc->pcp = pcp;
}

static void PaintCurvePointToTransData(PaintCurvePoint *pcp,
                                       TransData *td,
                                       TransData2D *td2d,
                                       TransDataPaintCurve *tdpc)
{
  BezTriple *bezt = &pcp->bez;

  if (pcp->bez.f2 == SELECT) {
    int i;
    for (i = 0; i < 3; i++) {
      copy_v2_v2(td2d->loc, bezt->vec[i]);
      td2d->loc[2] = 0.0f;
      td2d->loc2d = bezt->vec[i];

      td->flag = 0;
      td->loc = td2d->loc;
      copy_v3_v3(td->center, bezt->vec[1]);
      copy_v3_v3(td->iloc, td->loc);

      memset(td->axismtx, 0, sizeof(td->axismtx));
      td->axismtx[2][2] = 1.0f;

      td->ext = NULL;
      td->val = NULL;
      td->flag |= TD_SELECTED;
      td->dist = 0.0;

      unit_m3(td->mtx);
      unit_m3(td->smtx);

      tdpc->id = i;
      tdpc->pcp = pcp;

      td++;
      td2d++;
      tdpc++;
    }
  }
  else {
    if (bezt->f3 & SELECT) {
      PaintCurveConvertHandle(pcp, 2, td2d, tdpc, td);
      td2d++;
      tdpc++;
      td++;
    }

    if (bezt->f1 & SELECT) {
      PaintCurveConvertHandle(pcp, 0, td2d, tdpc, td);
    }
  }
}

static void createTransPaintCurveVerts(bContext *C, TransInfo *t)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  PaintCurve *pc;
  PaintCurvePoint *pcp;
  Brush *br;
  TransData *td = NULL;
  TransData2D *td2d = NULL;
  TransDataPaintCurve *tdpc = NULL;
  int i;
  int total = 0;

  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);

  tc->data_len = 0;

  if (!paint || !paint->brush || !paint->brush->paint_curve) {
    return;
  }

  br = paint->brush;
  pc = br->paint_curve;

  for (pcp = pc->points, i = 0; i < pc->tot_points; i++, pcp++) {
    if (PC_IS_ANY_SEL(pcp)) {
      if (pcp->bez.f2 & SELECT) {
        total += 3;
        continue;
      }
      else {
        if (pcp->bez.f1 & SELECT) {
          total++;
        }
        if (pcp->bez.f3 & SELECT) {
          total++;
        }
      }
    }
  }

  if (!total) {
    return;
  }

  tc->data_len = total;
  td2d = tc->data_2d = MEM_callocN(tc->data_len * sizeof(TransData2D), "TransData2D");
  td = tc->data = MEM_callocN(tc->data_len * sizeof(TransData), "TransData");
  tc->custom.type.data = tdpc = MEM_callocN(tc->data_len * sizeof(TransDataPaintCurve),
                                            "TransDataPaintCurve");
  tc->custom.type.use_free = true;

  for (pcp = pc->points, i = 0; i < pc->tot_points; i++, pcp++) {
    if (PC_IS_ANY_SEL(pcp)) {
      PaintCurvePointToTransData(pcp, td, td2d, tdpc);

      if (pcp->bez.f2 & SELECT) {
        td += 3;
        td2d += 3;
        tdpc += 3;
      }
      else {
        if (pcp->bez.f1 & SELECT) {
          td++;
          td2d++;
          tdpc++;
        }
        if (pcp->bez.f3 & SELECT) {
          td++;
          td2d++;
          tdpc++;
        }
      }
    }
  }
}

void flushTransPaintCurve(TransInfo *t)
{
  int i;

  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);

  TransData2D *td2d = tc->data_2d;
  TransDataPaintCurve *tdpc = tc->custom.type.data;

  for (i = 0; i < tc->data_len; i++, tdpc++, td2d++) {
    PaintCurvePoint *pcp = tdpc->pcp;
    copy_v2_v2(pcp->bez.vec[tdpc->id], td2d->loc);
  }
}

static void createTransGPencil_center_get(bGPDstroke *gps, float r_center[3])
{
  bGPDspoint *pt;
  int i;

  zero_v3(r_center);
  int tot_sel = 0;
  for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
    if (pt->flag & GP_SPOINT_SELECT) {
      add_v3_v3(r_center, &pt->x);
      tot_sel++;
    }
  }

  if (tot_sel > 0) {
    mul_v3_fl(r_center, 1.0f / tot_sel);
  }
}

static void createTransGPencil(bContext *C, TransInfo *t)
{
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  bGPdata *gpd = ED_gpencil_data_get_active(C);
  ToolSettings *ts = CTX_data_tool_settings(C);

  bool is_multiedit = (bool)GPENCIL_MULTIEDIT_SESSIONS_ON(gpd);
  bool use_multiframe_falloff = (ts->gp_sculpt.flag & GP_SCULPT_SETT_FLAG_FRAME_FALLOFF) != 0;

  Object *obact = CTX_data_active_object(C);
  bGPDlayer *gpl;
  TransData *td = NULL;
  float mtx[3][3], smtx[3][3];

  const Scene *scene = CTX_data_scene(C);
  const int cfra_scene = CFRA;

  const bool is_prop_edit = (t->flag & T_PROP_EDIT) != 0;
  const bool is_prop_edit_connected = (t->flag & T_PROP_CONNECTED) != 0;

  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);

  /* == Grease Pencil Strokes to Transform Data ==
   * Grease Pencil stroke points can be a mixture of 2D (screen-space),
   * or 3D coordinates. However, they're always saved as 3D points.
   * For now, we just do these without creating TransData2D for the 2D
   * strokes. This may cause issues in future though.
   */
  tc->data_len = 0;

  if (gpd == NULL) {
    return;
  }

  /* initialize falloff curve */
  if (is_multiedit) {
    BKE_curvemapping_initialize(ts->gp_sculpt.cur_falloff);
  }

  /* First Pass: Count the number of data-points required for the strokes,
   * (and additional info about the configuration - e.g. 2D/3D?).
   */
  for (gpl = gpd->layers.first; gpl; gpl = gpl->next) {
    /* only editable and visible layers are considered */
    if (gpencil_layer_is_editable(gpl) && (gpl->actframe != NULL)) {
      bGPDframe *gpf;
      bGPDstroke *gps;
      bGPDframe *init_gpf = gpl->actframe;
      if (is_multiedit) {
        init_gpf = gpl->frames.first;
      }

      for (gpf = init_gpf; gpf; gpf = gpf->next) {
        if ((gpf == gpl->actframe) || ((gpf->flag & GP_FRAME_SELECT) && (is_multiedit))) {
          for (gps = gpf->strokes.first; gps; gps = gps->next) {
            /* skip strokes that are invalid for current view */
            if (ED_gpencil_stroke_can_use(C, gps) == false) {
              continue;
            }
            /* check if the color is editable */
            if (ED_gpencil_stroke_color_use(obact, gpl, gps) == false) {
              continue;
            }

            if (is_prop_edit) {
              /* Proportional Editing... */
              if (is_prop_edit_connected) {
                /* connected only - so only if selected */
                if (gps->flag & GP_STROKE_SELECT) {
                  tc->data_len += gps->totpoints;
                }
              }
              else {
                /* everything goes - connection status doesn't matter */
                tc->data_len += gps->totpoints;
              }
            }
            else {
              /* only selected stroke points are considered */
              if (gps->flag & GP_STROKE_SELECT) {
                bGPDspoint *pt;
                int i;

                // TODO: 2D vs 3D?
                for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
                  if (pt->flag & GP_SPOINT_SELECT) {
                    tc->data_len++;
                  }
                }
              }
            }
          }
        }
        /* if not multiedit out of loop */
        if (!is_multiedit) {
          break;
        }
      }
    }
  }

  /* Stop trying if nothing selected */
  if (tc->data_len == 0) {
    return;
  }

  /* Allocate memory for data */
  tc->data = MEM_callocN(tc->data_len * sizeof(TransData), "TransData(GPencil)");
  td = tc->data;

  unit_m3(smtx);
  unit_m3(mtx);

  /* Second Pass: Build transdata array */
  for (gpl = gpd->layers.first; gpl; gpl = gpl->next) {
    /* only editable and visible layers are considered */
    if (gpencil_layer_is_editable(gpl) && (gpl->actframe != NULL)) {
      const int cfra = (gpl->flag & GP_LAYER_FRAMELOCK) ? gpl->actframe->framenum : cfra_scene;
      bGPDframe *gpf = gpl->actframe;
      bGPDstroke *gps;
      float diff_mat[4][4];
      float inverse_diff_mat[4][4];

      bGPDframe *init_gpf = gpl->actframe;
      if (is_multiedit) {
        init_gpf = gpl->frames.first;
      }
      /* init multiframe falloff options */
      int f_init = 0;
      int f_end = 0;

      if (use_multiframe_falloff) {
        BKE_gpencil_get_range_selected(gpl, &f_init, &f_end);
      }

      /* calculate difference matrix */
      ED_gpencil_parent_location(depsgraph, obact, gpd, gpl, diff_mat);
      /* undo matrix */
      invert_m4_m4(inverse_diff_mat, diff_mat);

      /* Make a new frame to work on if the layer's frame
       * and the current scene frame don't match up.
       *
       * - This is useful when animating as it saves that "uh-oh" moment when you realize you've
       *   spent too much time editing the wrong frame...
       */
      // XXX: should this be allowed when framelock is enabled?
      if ((gpf->framenum != cfra) && (!is_multiedit)) {
        gpf = BKE_gpencil_frame_addcopy(gpl, cfra);
        /* in some weird situations (framelock enabled) return NULL */
        if (gpf == NULL) {
          continue;
        }
        if (!is_multiedit) {
          init_gpf = gpf;
        }
      }

      /* Loop over strokes, adding TransData for points as needed... */
      for (gpf = init_gpf; gpf; gpf = gpf->next) {
        if ((gpf == gpl->actframe) || ((gpf->flag & GP_FRAME_SELECT) && (is_multiedit))) {

          /* if multiframe and falloff, recalculate and save value */
          float falloff = 1.0f; /* by default no falloff */
          if ((is_multiedit) && (use_multiframe_falloff)) {
            /* Faloff depends on distance to active frame (relative to the overall frame range) */
            falloff = BKE_gpencil_multiframe_falloff_calc(
                gpf, gpl->actframe->framenum, f_init, f_end, ts->gp_sculpt.cur_falloff);
          }

          for (gps = gpf->strokes.first; gps; gps = gps->next) {
            TransData *head = td;
            TransData *tail = td;
            bool stroke_ok;

            /* skip strokes that are invalid for current view */
            if (ED_gpencil_stroke_can_use(C, gps) == false) {
              continue;
            }
            /* check if the color is editable */
            if (ED_gpencil_stroke_color_use(obact, gpl, gps) == false) {
              continue;
            }
            /* What we need to include depends on proportional editing settings... */
            if (is_prop_edit) {
              if (is_prop_edit_connected) {
                /* A) "Connected" - Only those in selected strokes */
                stroke_ok = (gps->flag & GP_STROKE_SELECT) != 0;
              }
              else {
                /* B) All points, always */
                stroke_ok = true;
              }
            }
            else {
              /* C) Only selected points in selected strokes */
              stroke_ok = (gps->flag & GP_STROKE_SELECT) != 0;
            }

            /* Do stroke... */
            if (stroke_ok && gps->totpoints) {
              bGPDspoint *pt;
              int i;

              /* save falloff factor */
              gps->runtime.multi_frame_falloff = falloff;

              /* calculate stroke center */
              float center[3];
              createTransGPencil_center_get(gps, center);

              /* add all necessary points... */
              for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
                bool point_ok;

                /* include point? */
                if (is_prop_edit) {
                  /* Always all points in strokes that get included */
                  point_ok = true;
                }
                else {
                  /* Only selected points in selected strokes */
                  point_ok = (pt->flag & GP_SPOINT_SELECT) != 0;
                }

                /* do point... */
                if (point_ok) {
                  copy_v3_v3(td->iloc, &pt->x);
                  /* only copy center in local origins.
                   * This allows get interesting effects also when move
                   * using proportional editing */
                  if ((gps->flag & GP_STROKE_SELECT) &&
                      (ts->transform_pivot_point == V3D_AROUND_LOCAL_ORIGINS)) {
                    copy_v3_v3(td->center, center);
                  }
                  else {
                    copy_v3_v3(td->center, &pt->x);
                  }

                  td->loc = &pt->x;

                  td->flag = 0;

                  if (pt->flag & GP_SPOINT_SELECT) {
                    td->flag |= TD_SELECTED;
                  }

                  /* for other transform modes (e.g. shrink-fatten), need to additional data
                   * but never for scale or mirror
                   */
                  if ((t->mode != TFM_RESIZE) && (t->mode != TFM_MIRROR)) {
                    if (t->mode != TFM_GPENCIL_OPACITY) {
                      td->val = &pt->pressure;
                      td->ival = pt->pressure;
                    }
                    else {
                      td->val = &pt->strength;
                      td->ival = pt->strength;
                    }
                  }

                  /* screenspace needs special matrices... */
                  if ((gps->flag & (GP_STROKE_3DSPACE | GP_STROKE_2DSPACE | GP_STROKE_2DIMAGE)) ==
                      0) {
                    /* screenspace */
                    td->protectflag = OB_LOCK_LOCZ | OB_LOCK_ROTZ | OB_LOCK_SCALEZ;
                  }
                  else {
                    /* configure 2D dataspace points so that they don't play up... */
                    if (gps->flag & (GP_STROKE_2DSPACE | GP_STROKE_2DIMAGE)) {
                      td->protectflag = OB_LOCK_LOCZ | OB_LOCK_ROTZ | OB_LOCK_SCALEZ;
                    }
                  }
                  /* apply parent transformations */
                  copy_m3_m4(td->smtx, inverse_diff_mat); /* final position */
                  copy_m3_m4(td->mtx, diff_mat);          /* display position */
                  copy_m3_m4(td->axismtx, diff_mat);      /* axis orientation */

                  /* Triangulation must be calculated again,
                   * so save the stroke for recalc function */
                  td->extra = gps;

                  /* save pointer to object */
                  td->ob = obact;

                  td++;
                  tail++;
                }
              }

              /* March over these points, and calculate the proportional editing distances */
              if (is_prop_edit && (head != tail)) {
                /* XXX: for now, we are similar enough that this works... */
                calc_distanceCurveVerts(head, tail - 1);
              }
            }
          }
        }
        /* if not multiedit out of loop */
        if (!is_multiedit) {
          break;
        }
      }
    }
  }
}

static int countAndCleanTransDataContainer(TransInfo *t)
{
  BLI_assert(ELEM(t->data_len_all, 0, -1));
  t->data_len_all = 0;
  uint data_container_len_orig = t->data_container_len;
  for (TransDataContainer *th_end = t->data_container - 1,
                          *tc = t->data_container + (t->data_container_len - 1);
       tc != th_end;
       tc--) {
    if (tc->data_len == 0) {
      uint index = tc - t->data_container;
      if (index + 1 != t->data_container_len) {
        SWAP(TransDataContainer,
             t->data_container[index],
             t->data_container[t->data_container_len - 1]);
      }
      t->data_container_len -= 1;
    }
    else {
      t->data_len_all += tc->data_len;
    }
  }
  if (data_container_len_orig != t->data_container_len) {
    t->data_container = MEM_reallocN(t->data_container,
                                     sizeof(*t->data_container) * t->data_container_len);
  }
  return t->data_len_all;
}

void createTransData(bContext *C, TransInfo *t)
{
  Scene *scene = t->scene;
  ViewLayer *view_layer = t->view_layer;
  Object *ob = OBACT(view_layer);

  bool has_transform_context = true;
  t->data_len_all = -1;

  /* if tests must match recalcData for correct updates */
  if (t->options & CTX_CURSOR) {
    t->flag |= T_CURSOR;
    t->obedit_type = -1;

    if (t->spacetype == SPACE_IMAGE) {
      createTransCursor_image(t);
    }
    else {
      createTransCursor_view3d(t);
    }
    countAndCleanTransDataContainer(t);
  }
  else if (t->options & CTX_TEXTURE) {
    t->flag |= T_TEXTURE;
    t->obedit_type = -1;

    createTransTexspace(t);
    countAndCleanTransDataContainer(t);
  }
  else if (t->options & CTX_EDGE) {
    /* Multi object editing. */
    initTransDataContainers_FromObjectData(t, ob, NULL, 0);
    FOREACH_TRANS_DATA_CONTAINER (t, tc) {
      tc->data_ext = NULL;
    }
    t->flag |= T_EDIT;

    createTransEdge(t);
    countAndCleanTransDataContainer(t);

    if (t->data_len_all && t->flag & T_PROP_EDIT) {
      sort_trans_data_selected_first(t);
      set_prop_dist(t, 1);
      sort_trans_data_dist(t);
    }
  }
  else if (t->options & CTX_GPENCIL_STROKES) {
    t->options |= CTX_GPENCIL_STROKES;
    t->flag |= T_POINTS | T_EDIT;

    initTransDataContainers_FromObjectData(t, ob, NULL, 0);
    createTransGPencil(C, t);
    countAndCleanTransDataContainer(t);

    if (t->data_len_all && (t->flag & T_PROP_EDIT)) {
      sort_trans_data_selected_first(t);
      set_prop_dist(t, 1);
      sort_trans_data_dist(t);
    }
  }
  else if (t->spacetype == SPACE_IMAGE) {
    t->flag |= T_POINTS | T_2D_EDIT;
    if (t->options & CTX_MASK) {

      /* copied from below */
      createTransMaskingData(C, t);
      countAndCleanTransDataContainer(t);

      if (t->data_len_all && (t->flag & T_PROP_EDIT)) {
        sort_trans_data_selected_first(t);
        set_prop_dist(t, true);
        sort_trans_data_dist(t);
      }
    }
    else if (t->options & CTX_PAINT_CURVE) {
      if (!ELEM(t->mode, TFM_SHEAR, TFM_SHRINKFATTEN)) {
        createTransPaintCurveVerts(C, t);
        countAndCleanTransDataContainer(t);
      }
      else {
        has_transform_context = false;
      }
    }
    else if (t->obedit_type == OB_MESH) {

      initTransDataContainers_FromObjectData(t, ob, NULL, 0);
      createTransUVs(C, t);
      countAndCleanTransDataContainer(t);

      t->flag |= T_EDIT;

      if (t->data_len_all && (t->flag & T_PROP_EDIT)) {
        sort_trans_data_selected_first(t);
        set_prop_dist(t, 1);
        sort_trans_data_dist(t);
      }
    }
    else {
      has_transform_context = false;
    }
  }
  else if (t->spacetype == SPACE_ACTION) {
    t->flag |= T_POINTS | T_2D_EDIT;
    t->obedit_type = -1;

    createTransActionData(C, t);
    countAndCleanTransDataContainer(t);

    if (t->data_len_all && (t->flag & T_PROP_EDIT)) {
      sort_trans_data_selected_first(t);
      /* don't do that, distance has been set in createTransActionData already */
      // set_prop_dist(t, false);
      sort_trans_data_dist(t);
    }
  }
  else if (t->spacetype == SPACE_NLA) {
    t->flag |= T_POINTS | T_2D_EDIT;
    t->obedit_type = -1;

    createTransNlaData(C, t);
    countAndCleanTransDataContainer(t);
  }
  else if (t->spacetype == SPACE_SEQ) {
    t->flag |= T_POINTS | T_2D_EDIT;
    t->obedit_type = -1;

    t->num.flag |= NUM_NO_FRACTION; /* sequencer has no use for floating point trasnform */
    createTransSeqData(C, t);
    countAndCleanTransDataContainer(t);
  }
  else if (t->spacetype == SPACE_GRAPH) {
    t->flag |= T_POINTS | T_2D_EDIT;
    t->obedit_type = -1;

    createTransGraphEditData(C, t);
    countAndCleanTransDataContainer(t);

    if (t->data_len_all && (t->flag & T_PROP_EDIT)) {
      /* makes selected become first in array */
      sort_trans_data_selected_first(t);

      /* don't do that, distance has been set in createTransGraphEditData already */
      set_prop_dist(t, false);

      sort_trans_data_dist(t);
    }
  }
  else if (t->spacetype == SPACE_NODE) {
    t->flag |= T_POINTS | T_2D_EDIT;
    t->obedit_type = -1;

    createTransNodeData(C, t);
    countAndCleanTransDataContainer(t);

    if (t->data_len_all && (t->flag & T_PROP_EDIT)) {
      sort_trans_data_selected_first(t);
      set_prop_dist(t, 1);
      sort_trans_data_dist(t);
    }
  }
  else if (t->spacetype == SPACE_CLIP) {
    t->flag |= T_POINTS | T_2D_EDIT;
    t->obedit_type = -1;

    if (t->options & CTX_MOVIECLIP) {
      createTransTrackingData(C, t);
      countAndCleanTransDataContainer(t);
    }
    else if (t->options & CTX_MASK) {
      /* copied from above */
      createTransMaskingData(C, t);
      countAndCleanTransDataContainer(t);

      if (t->data_len_all && (t->flag & T_PROP_EDIT)) {
        sort_trans_data_selected_first(t);
        set_prop_dist(t, true);
        sort_trans_data_dist(t);
      }
    }
    else {
      has_transform_context = false;
    }
  }
  else if (t->obedit_type != -1) {
    /* Multi object editing. */
    initTransDataContainers_FromObjectData(t, ob, NULL, 0);

    FOREACH_TRANS_DATA_CONTAINER (t, tc) {
      tc->data_ext = NULL;
    }
    if (t->obedit_type == OB_MESH) {
      createTransEditVerts(t);
    }
    else if (ELEM(t->obedit_type, OB_CURVE, OB_SURF)) {
      createTransCurveVerts(t);
    }
    else if (t->obedit_type == OB_LATTICE) {
      createTransLatticeVerts(t);
    }
    else if (t->obedit_type == OB_MBALL) {
      createTransMBallVerts(t);
    }
    else if (t->obedit_type == OB_ARMATURE) {
      t->flag &= ~T_PROP_EDIT;
      createTransArmatureVerts(t);
    }
    else {
      printf("edit type not implemented!\n");
    }

    countAndCleanTransDataContainer(t);

    t->flag |= T_EDIT | T_POINTS;

    if (t->data_len_all) {
      if (t->flag & T_PROP_EDIT) {
        if (ELEM(t->obedit_type, OB_CURVE, OB_MESH)) {
          sort_trans_data_selected_first(t);
          if ((t->obedit_type == OB_MESH) && (t->flag & T_PROP_CONNECTED)) {
            /* already calculated by editmesh_set_connectivity_distance */
          }
          else {
            set_prop_dist(t, 0);
          }
          sort_trans_data_dist(t);
        }
        else {
          sort_trans_data_selected_first(t);
          set_prop_dist(t, 1);
          sort_trans_data_dist(t);
        }
      }
      else {
        if (ELEM(t->obedit_type, OB_CURVE)) {
          /* Needed because bezier handles can be partially selected
           * and are still added into transform data. */
          sort_trans_data_selected_first(t);
        }
      }
    }

    /* exception... hackish, we want bonesize to use bone orientation matrix (ton) */
    if (t->mode == TFM_BONESIZE) {
      t->flag &= ~(T_EDIT | T_POINTS);
      t->flag |= T_POSE;
      t->obedit_type = -1;

      FOREACH_TRANS_DATA_CONTAINER (t, tc) {
        tc->poseobj = tc->obedit;
        tc->obedit = NULL;
      }
    }
  }
  else if (ob && (ob->mode & OB_MODE_POSE)) {
    /* XXX this is currently limited to active armature only... */

    /* XXX active-layer checking isn't done
     * as that should probably be checked through context instead. */

    /* Multi object editing. */
    initTransDataContainers_FromObjectData(t, ob, NULL, 0);
    createTransPose(t);
    countAndCleanTransDataContainer(t);
  }
  else if (ob && (ob->mode & OB_MODE_WEIGHT_PAINT) && !(t->options & CTX_PAINT_CURVE)) {
    /* important that ob_armature can be set even when its not selected [#23412]
     * lines below just check is also visible */
    has_transform_context = false;
    Object *ob_armature = modifiers_isDeformedByArmature(ob);
    if (ob_armature && ob_armature->mode & OB_MODE_POSE) {
      Base *base_arm = BKE_view_layer_base_find(t->view_layer, ob_armature);
      if (base_arm) {
        View3D *v3d = t->view;
        if (BASE_VISIBLE(v3d, base_arm)) {
          Object *objects[1];
          objects[0] = ob_armature;
          uint objects_len = 1;
          initTransDataContainers_FromObjectData(t, ob_armature, objects, objects_len);
          createTransPose(t);
          countAndCleanTransDataContainer(t);
          has_transform_context = true;
        }
      }
    }
  }
  else if (ob && (ob->mode & OB_MODE_PARTICLE_EDIT) && PE_start_edit(PE_get_current(scene, ob))) {
    createTransParticleVerts(C, t);
    countAndCleanTransDataContainer(t);
    t->flag |= T_POINTS;

    if (t->data_len_all && t->flag & T_PROP_EDIT) {
      sort_trans_data_selected_first(t);
      set_prop_dist(t, 1);
      sort_trans_data_dist(t);
    }
  }
  else if (ob && (ob->mode & OB_MODE_ALL_PAINT)) {
    if ((t->options & CTX_PAINT_CURVE) && !ELEM(t->mode, TFM_SHEAR, TFM_SHRINKFATTEN)) {
      t->flag |= T_POINTS | T_2D_EDIT;
      createTransPaintCurveVerts(C, t);
      countAndCleanTransDataContainer(t);
    }
    else {
      has_transform_context = false;
    }
  }
  else if ((ob) &&
           (ELEM(
               ob->mode, OB_MODE_PAINT_GPENCIL, OB_MODE_SCULPT_GPENCIL, OB_MODE_WEIGHT_GPENCIL))) {
    /* In grease pencil all transformations must be canceled if not Object or Edit. */
    has_transform_context = false;
  }
  else {
    /* Needed for correct Object.obmat after duplication, see: T62135. */
    BKE_scene_graph_evaluated_ensure(t->depsgraph, CTX_data_main(t->context));

    if ((scene->toolsettings->transform_flag & SCE_XFORM_DATA_ORIGIN) != 0) {
      t->options |= CTX_OBMODE_XFORM_OBDATA;
    }
    if ((scene->toolsettings->transform_flag & SCE_XFORM_SKIP_CHILDREN) != 0) {
      t->options |= CTX_OBMODE_XFORM_SKIP_CHILDREN;
    }

    createTransObject(C, t);
    countAndCleanTransDataContainer(t);
    t->flag |= T_OBJECT;

    if (t->data_len_all && t->flag & T_PROP_EDIT) {
      // selected objects are already first, no need to presort
      set_prop_dist(t, 1);
      sort_trans_data_dist(t);
    }

    /* Check if we're transforming the camera from the camera */
    if ((t->spacetype == SPACE_VIEW3D) && (t->ar->regiontype == RGN_TYPE_WINDOW)) {
      View3D *v3d = t->view;
      RegionView3D *rv3d = t->ar->regiondata;
      if ((rv3d->persp == RV3D_CAMOB) && v3d->camera) {
        /* we could have a flag to easily check an object is being transformed */
        if (v3d->camera->id.tag & LIB_TAG_DOIT) {
          t->flag |= T_CAMERA;
        }
      }
    }
  }

  /* Check that 'countAndCleanTransDataContainer' ran. */
  if (has_transform_context) {
    BLI_assert(t->data_len_all != -1);
  }
  else {
    BLI_assert(t->data_len_all == -1);
    t->data_len_all = 0;
  }

  BLI_assert((!(t->flag & T_EDIT)) == (!(t->obedit_type != -1)));
}
