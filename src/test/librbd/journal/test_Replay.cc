// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "test/librbd/test_fixture.h"
#include "test/librbd/test_support.h"
#include "cls/journal/cls_journal_types.h"
#include "cls/journal/cls_journal_client.h"
#include "journal/Journaler.h"
#include "librbd/AioCompletion.h"
#include "librbd/AioImageRequest.h"
#include "librbd/AioImageRequestWQ.h"
#include "librbd/ExclusiveLock.h"
#include "librbd/ImageCtx.h"
#include "librbd/ImageWatcher.h"
#include "librbd/Journal.h"
#include "librbd/journal/Types.h"

void register_test_journal_replay() {
}

class TestJournalReplay : public TestFixture {
public:

  int when_acquired_lock(librbd::ImageCtx *ictx) {
    C_SaferCond lock_ctx;
    {
      RWLock::WLocker owner_locker(ictx->owner_lock);
      ictx->exclusive_lock->request_lock(&lock_ctx);
    }
    return lock_ctx.wait();
  }

  template<typename T>
  void inject_into_journal(librbd::ImageCtx *ictx, T event) {

    librbd::journal::EventEntry event_entry(event);
    librbd::Journal<>::AioObjectRequests requests;
    {
      RWLock::RLocker owner_locker(ictx->owner_lock);
      ictx->journal->append_io_event(NULL, std::move(event_entry), requests, 0,
				     0, true);
    }
  }

  void get_journal_commit_position(librbd::ImageCtx *ictx, int64_t *tid)
  {
    const std::string client_id = "";
    std::string journal_id = ictx->id;

    C_SaferCond close_cond;
    ictx->journal->close(&close_cond);
    ASSERT_EQ(0, close_cond.wait());
    delete ictx->journal;
    ictx->journal = nullptr;

    C_SaferCond cond;
    uint64_t minimum_set;
    uint64_t active_set;
    std::set<cls::journal::Client> registered_clients;
    std::string oid = ::journal::Journaler::header_oid(journal_id);
    cls::journal::client::get_mutable_metadata(ictx->md_ctx, oid, &minimum_set,
	&active_set, &registered_clients, &cond);
    ASSERT_EQ(0, cond.wait());
    std::set<cls::journal::Client>::const_iterator c;
    for (c = registered_clients.begin(); c != registered_clients.end(); ++c) {
      if (c->id == client_id) {
	break;
      }
    }
    if (c == registered_clients.end()) {
      *tid = -1;
      return;
    }
    cls::journal::EntryPositions entry_positions =
	c->commit_position.entry_positions;
    cls::journal::EntryPositions::const_iterator p;
    for (p = entry_positions.begin(); p != entry_positions.end(); ++p) {
      if (p->tag_tid == 0) {
	break;
      }
    }
    *tid = p == entry_positions.end() ? -1 : p->entry_tid;

    C_SaferCond open_cond;
    ictx->journal = new librbd::Journal<>(*ictx);
    ictx->journal->open(&open_cond);
    ASSERT_EQ(0, open_cond.wait());
  }
};

TEST_F(TestJournalReplay, AioDiscardEvent) {
  REQUIRE_FEATURE(RBD_FEATURE_JOURNALING);

  // write to the image w/o using the journal
  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  ictx->features &= ~RBD_FEATURE_JOURNALING;

  std::string payload(4096, '1');
  librbd::AioCompletion *aio_comp = new librbd::AioCompletion();
  ictx->aio_work_queue->aio_write(aio_comp, 0, payload.size(), payload.c_str(),
                                  0);
  ASSERT_EQ(0, aio_comp->wait_for_complete());
  aio_comp->release();

  aio_comp = new librbd::AioCompletion();
  ictx->aio_work_queue->aio_flush(aio_comp);
  ASSERT_EQ(0, aio_comp->wait_for_complete());
  aio_comp->release();

  std::string read_payload(4096, '\0');
  aio_comp = new librbd::AioCompletion();
  ictx->aio_work_queue->aio_read(aio_comp, 0, read_payload.size(),
                                 &read_payload[0], NULL, 0);
  ASSERT_EQ(0, aio_comp->wait_for_complete());
  aio_comp->release();
  ASSERT_EQ(payload, read_payload);
  close_image(ictx);

  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  ASSERT_EQ(0, when_acquired_lock(ictx));

  // get current commit position
  int64_t initial;
  get_journal_commit_position(ictx, &initial);

  // inject a discard operation into the journal
  inject_into_journal(ictx,
                      librbd::journal::AioDiscardEvent(0, payload.size()));

  // re-open the journal so that it replays the new entry
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  ASSERT_EQ(0, when_acquired_lock(ictx));

  aio_comp = new librbd::AioCompletion();
  ictx->aio_work_queue->aio_read(aio_comp, 0, read_payload.size(),
                                 &read_payload[0], NULL, 0);
  ASSERT_EQ(0, aio_comp->wait_for_complete());
  aio_comp->release();
  ASSERT_EQ(std::string(read_payload.size(), '\0'), read_payload);

  // check the commit position is properly updated
  int64_t current;
  get_journal_commit_position(ictx, &current);
  ASSERT_EQ(current, initial + 1);

  // replay several envents and check the commit position
  inject_into_journal(ictx,
                      librbd::journal::AioDiscardEvent(0, payload.size()));
  inject_into_journal(ictx,
                      librbd::journal::AioDiscardEvent(0, payload.size()));
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  ASSERT_EQ(0, when_acquired_lock(ictx));
  get_journal_commit_position(ictx, &current);
  ASSERT_EQ(current, initial + 3);
}

TEST_F(TestJournalReplay, AioWriteEvent) {
  REQUIRE_FEATURE(RBD_FEATURE_JOURNALING);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  ASSERT_EQ(0, when_acquired_lock(ictx));

  // get current commit position
  int64_t initial;
  get_journal_commit_position(ictx, &initial);

  // inject a write operation into the journal
  std::string payload(4096, '1');
  bufferlist payload_bl;
  payload_bl.append(payload);
  inject_into_journal(ictx,
      librbd::journal::AioWriteEvent(0, payload.size(), payload_bl));

  // re-open the journal so that it replays the new entry
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  ASSERT_EQ(0, when_acquired_lock(ictx));

  std::string read_payload(4096, '\0');
  librbd::AioCompletion *aio_comp = new librbd::AioCompletion();
  ictx->aio_work_queue->aio_read(aio_comp, 0, read_payload.size(),
                                 &read_payload[0], NULL, 0);
  ASSERT_EQ(0, aio_comp->wait_for_complete());
  aio_comp->release();
  ASSERT_EQ(payload, read_payload);

  // check the commit position is properly updated
  int64_t current;
  get_journal_commit_position(ictx, &current);
  ASSERT_EQ(current, initial + 1);

  // replay several events and check the commit position
  inject_into_journal(ictx,
      librbd::journal::AioWriteEvent(0, payload.size(), payload_bl));
  inject_into_journal(ictx,
      librbd::journal::AioWriteEvent(0, payload.size(), payload_bl));
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  ASSERT_EQ(0, when_acquired_lock(ictx));
  get_journal_commit_position(ictx, &current);
  ASSERT_EQ(current, initial + 3);
}

TEST_F(TestJournalReplay, AioFlushEvent) {
  REQUIRE_FEATURE(RBD_FEATURE_JOURNALING);

  librbd::ImageCtx *ictx;

  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  ASSERT_EQ(0, when_acquired_lock(ictx));

  // get current commit position
  int64_t initial;
  get_journal_commit_position(ictx, &initial);

  // inject a flush operation into the journal
  inject_into_journal(ictx, librbd::journal::AioFlushEvent());

  // start an AIO write op
  librbd::Journal<> *journal = ictx->journal;
  ictx->journal = NULL;

  std::string payload(m_image_size, '1');
  librbd::AioCompletion *aio_comp = new librbd::AioCompletion();
  {
    RWLock::RLocker owner_lock(ictx->owner_lock);
    librbd::AioImageRequest<>::aio_write(ictx, aio_comp, 0, payload.size(),
                                         payload.c_str(), 0);
  }
  ictx->journal = journal;

  // re-open the journal so that it replays the new entry
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  ASSERT_EQ(0, when_acquired_lock(ictx));

  ASSERT_TRUE(aio_comp->is_complete());
  ASSERT_EQ(0, aio_comp->wait_for_complete());
  aio_comp->release();

  std::string read_payload(m_image_size, '\0');
  aio_comp = new librbd::AioCompletion();
  ictx->aio_work_queue->aio_read(aio_comp, 0, read_payload.size(),
                                 &read_payload[0], NULL, 0);
  ASSERT_EQ(0, aio_comp->wait_for_complete());
  aio_comp->release();
  ASSERT_EQ(payload, read_payload);

  // check the commit position is properly updated
  int64_t current;
  get_journal_commit_position(ictx, &current);
  ASSERT_EQ(current, initial + 1);

  // replay several events and check the commit position
  inject_into_journal(ictx, librbd::journal::AioFlushEvent());
  inject_into_journal(ictx, librbd::journal::AioFlushEvent());
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  ASSERT_EQ(0, when_acquired_lock(ictx));
  get_journal_commit_position(ictx, &current);
  ASSERT_EQ(current, initial + 3);
}

TEST_F(TestJournalReplay, EntryPosition) {
  REQUIRE_FEATURE(RBD_FEATURE_JOURNALING);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  ASSERT_EQ(0, when_acquired_lock(ictx));

  // get current commit position
  int64_t initial;
  get_journal_commit_position(ictx, &initial);

  std::string payload(4096, '1');
  librbd::AioCompletion *aio_comp = new librbd::AioCompletion();
  ictx->aio_work_queue->aio_write(aio_comp, 0, payload.size(), payload.c_str(),
                                  0);
  ASSERT_EQ(0, aio_comp->wait_for_complete());
  aio_comp->release();

  aio_comp = new librbd::AioCompletion();
  ictx->aio_work_queue->aio_flush(aio_comp);
  ASSERT_EQ(0, aio_comp->wait_for_complete());
  aio_comp->release();

  // check the commit position updated
  int64_t current;
  get_journal_commit_position(ictx, &current);
  ASSERT_EQ(current, initial + 2);

  // write again

  aio_comp = new librbd::AioCompletion();
  ictx->aio_work_queue->aio_write(aio_comp, 0, payload.size(), payload.c_str(),
                                  0);
  ASSERT_EQ(0, aio_comp->wait_for_complete());
  aio_comp->release();

  aio_comp = new librbd::AioCompletion();
  ictx->aio_work_queue->aio_flush(aio_comp);
  ASSERT_EQ(0, aio_comp->wait_for_complete());
  aio_comp->release();

  // check the commit position updated
  get_journal_commit_position(ictx, &current);
  ASSERT_EQ(current, initial + 4);
}
