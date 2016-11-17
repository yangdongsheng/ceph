// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_IMAGE_CLONE_REQUEST_H
#define CEPH_LIBRBD_IMAGE_CLONE_REQUEST_H

#include "common/WorkQueue.h"
#include "include/rados/librados.hpp"
#include "include/rbd_types.h"
#include "cls/rbd/cls_rbd_types.h"
#include "include/rbd/librbd.hpp"
#include "librbd/ImageCtx.h"
#include "librbd/internal.h"
#include "librbd/image/CreateRequest.h"

class Context;

using librados::IoCtx;

namespace librbd {
namespace image {

template <typename ImageCtxT = ImageCtx>
class CloneRequest {
public:
  static CloneRequest *create(ImageCtxT *p_imctx, IoCtx &c_ioctx, const std::string &c_name,
			      ImageOptions c_options,
			      const std::string &non_primary_global_image_id,
			      const std::string &primary_mirror_uuid,
			      ContextWQ *op_work_queue, Context *on_finish) {
    return new CloneRequest(p_imctx, c_ioctx, c_name, c_options,
                             non_primary_global_image_id, primary_mirror_uuid,
                             op_work_queue, on_finish);
  }

  void send();
private:
  /**
   * @verbatim
   *
   *                                  <start>
   *                                     |
   *                                     v
   *                             VALIDATE PARENT
   *                                     |
   *                                     v
   * (error: bottom up)           VALIDATE CHILD
   *  _______<_______                    |
   * |               |                   v
   * |               |             CREATE IMAGE
   * |               |                   |
   * |               |                   v          (parent_md exists)
   * |               |              OPEN IMAGE. . . . . > . . . .
   * v               |               /   |                      .
   * |         REMOVE IMAGE<--------/    v                      .
   * |               |           SET PARENT IN HEADER           .
   * |          CLOSE IMAGE          /   |                      .
   * |               ^-------<------/    v                      .
   * |               |\           UPDATE DIR_CHILDREN. . < . . . 
   * |               | \              /  |
   * |               |  *<-----------/   v
   * |               |                REFRESH
   * |               |                /  |                     
   * |   CLEAN DIR_CHILDREN <--------/   v            (meta is empty)
   * |               |\         GET METAS IN PARENT . . . . . . .
   * |               | \              /  |                      .
   * v               |  *<-----------/   v                      .
   * |               |          SET METAS IN CHILD              v
   * |               |               /   |                      .
   * |               -------<-------/    v                      .
   * |                               CLOSE IMAGE . . . . .< . . .
   * |                                   |
   * |                                   v
   * |_____________>__________________<finish>
   *
   * @endverbatim
   */

  CloneRequest(ImageCtxT *p_imctx, IoCtx &c_ioctx, const std::string &c_name,
			      ImageOptions c_options,
			      const std::string &non_primary_global_image_id,
			      const std::string &primary_mirror_uuid,
			      ContextWQ *op_work_queue, Context *on_finish);

  ImageCtxT *m_p_imctx;
  IoCtx &m_ioctx;
  std::string m_name;
  ImageOptions m_opts;
  parent_spec m_pspec;
  ImageCtxT *m_imctx;
  const std::string m_non_primary_global_image_id;
  const std::string m_primary_mirror_uuid;
  ContextWQ *m_op_work_queue;
  Context *m_on_finish;
  librbd::NoOpProgressContext m_no_op;
  CreateRequest<ImageCtxT> *m_create_req;

  CephContext *m_cct;
  bool m_use_p_features;
  uint64_t m_p_features;
  uint64_t m_features;
  map<string, bufferlist> m_pairs;
  bufferlist m_out_bl;
  uint64_t m_size;
  int m_r_saved = 0;
  bool m_is_primary;
  bool m_force_non_primary;

  void validate_options();

  void send_validate_parent();
  void handle_validate_parent(int r);

  void send_validate_child();
  void handle_validate_child(int r);

  void send_create();
  void handle_create(int r);

  void send_open();
  void handle_open(int r);

  void send_set_parent();
  void handle_set_parent(int r);

  void send_add_child();
  void handle_add_child(int r);

  void send_refresh();
  void handle_refresh(int r);

  void send_metadata_list();
  void handle_metadata_list(int r);

  void send_metadata_set();
  void handle_metadata_set(int r);

  void send_close();
  void handle_close(int r);

  void switch_thread_context();
  void handle_switch_thread_context(int r);

  void send_remove();
  void handle_remove(int r);

  void send_remove_child();
  void handle_remove_child(int r);

  void complete(int r);
};

} //namespace image
} //namespace librbd

extern template class librbd::image::CloneRequest<librbd::ImageCtx>;

#endif // CEPH_LIBRBD_IMAGE_CLONE_REQUEST_H
