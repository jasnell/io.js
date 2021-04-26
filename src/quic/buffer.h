#ifndef SRC_QUIC_BUFFER_H_
#define SRC_QUIC_BUFFER_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include "async_wrap.h"
#include "base_object.h"
#include "memory_tracker.h"
#include "node.h"
#include "node_bob.h"
#include "node_file.h"
#include "node_internals.h"
#include "stream_base.h"
#include "util-inl.h"
#include "uv.h"

#include "ngtcp2/ngtcp2.h"

#include <deque>

namespace node {
namespace quic {

class Buffer;
class Stream;

constexpr size_t kMaxVectorCount = 16;

// When data is sent over QUIC, we are required to retain it in memory
// until we receive an acknowledgement that it has been successfully
// received by the peer. The QuicBuffer object is what we use to handle
// that and track until it is acknowledged. To understand the QuicBuffer
// object itself, it is important to understand how ngtcp2 and nghttp3
// handle data that is given to it to serialize into QUIC packets.
//
// An individual QUIC packet may contain multiple QUIC frames. Whenever
// we create a QUIC packet, we really have no idea what frames are going
// to be encoded or how much buffered handshake or stream data is going
// to be included within that QuicPacket. If there is buffered data
// available for a stream, we provide an array of pointers to that data
// and an indication about how much data is available, then we leave it
// entirely up to ngtcp2 and nghttp3 to determine how much of the data
// to encode into the QUIC packet. It is only *after* the QUIC packet
// is encoded that we can know how much was actually written.
//
// Once written to a QUIC Packet, we have to keep the data in memory
// until an acknowledgement is received. In QUIC, acknowledgements are
// received per range of packets, but (fortunately) ngtcp2 gives us that
// information as byte offsets instead.
//
// Buffer is complicated because it needs to be able to accomplish
// three things: (a) buffering v8::BackingStore instances passed down
// from JavaScript without memcpy, (b) tracking what data has already been
// encoded in a QUIC packet and what data is remaining to be read, and
// (c) tracking which data has been acknowledged and which hasn't.
//
// Buffer contains a deque of Buffer::Chunk instances.
// A single Buffer::Chunk wraps a v8::BackingStore with length and
// offset. When the Buffer::Chunk is created, we capture the total
// length of the buffer and the total number of bytes remaining to be sent.
// Initially, these numbers are identical.
//
// When data is encoded into a Packet, we advance the Buffer::Chunk's
// remaining-to-be-read by the number of bytes actually encoded. If there
// are no more bytes remaining to be encoded, we move to the next chunk
// in the deque (but we do not yet pop it off the deque).
//
// When an acknowledgement is received, we decrement the Buffer::Chunk's
// length by the number of acknowledged bytes. Once the unacknowledged
// length reaches 0 we pop the chunk off the deque.

class Buffer : public bob::SourceImpl<ngtcp2_vec>,
               public MemoryRetainer {
 public:
  // Stores chunks of both inbound and outbound data. Each chunk
  // stores a shared pointer to a v8::BackingStore with appropriate
  // length and offset details. Each Buffer::Chunk is stored in a
  // deque in Buffer which manages the aggregate collection of all chunks.
  class Chunk : public MemoryRetainer {
   public:
    static std::unique_ptr<Chunk> Create(
        Environment* env,
        const uint8_t* data,
        size_t len);

    static std::unique_ptr<Chunk> Create(
        const std::shared_ptr<v8::BackingStore>& data,
        size_t offset,
        size_t length);

    size_t Acknowledge(size_t amount);

    // Releases the chunk to a v8 Uint8Array. data_ is reset
    // and offset_, length_, and consumed_ are all set to 0
    // and the strong_ptr_, if any, is reset. This is used
    // only for inbound data and only when queued data is
    // being flushed out to the JavaScript side.
    v8::MaybeLocal<v8::Value> Release(Environment* env);

    // Increments consumed_ by amount bytes. If amount is greater
    // than remaining(), remaining() bytes are advanced. Returns
    // the actual number of bytes advanced.
    size_t Seek(size_t amount);

    // Returns a pointer to the remaining data. This is used only
    // for outbound data.
    const uint8_t* data() const;

    inline size_t length() const { return unacknowledged_; }

    inline size_t remaining() const { return length_ - read_; }

    ngtcp2_vec vec() const;

    void MemoryInfo(MemoryTracker* tracker) const override;
    SET_MEMORY_INFO_NAME(Buffer::Chunk)
    SET_SELF_SIZE(Chunk)

    using Queue = std::deque<std::unique_ptr<Buffer::Chunk>>;

   private:
    Chunk(
      const std::shared_ptr<v8::BackingStore>& data,
      size_t length,
      size_t offset = 0);

    std::shared_ptr<v8::BackingStore> data_;
    size_t offset_ = 0;
    size_t length_ = 0;
    size_t read_ = 0;
    size_t unacknowledged_ = 0;
  };

  // Receives the inbound data for a Stream
  struct Consumer {
    virtual v8::Maybe<size_t> Process(
        Chunk::Queue queue,
        bool ended = false) = 0;
  };

  // Provides outbound data for a stream
  class Source : public bob::SourceImpl<ngtcp2_vec>,
                 public MemoryRetainer {
   public:
    enum InternalFields {
      kSlot = BaseObject::kSlot,
      kSourceField = BaseObject::kInternalFieldCount,
      kInternalFieldCount
    };

    virtual BaseObjectPtr<BaseObject> GetStrongPtr() {
      return BaseObjectPtr<BaseObject>();
    }

    virtual size_t Acknowledge(uint64_t offset, size_t amount) = 0;
    virtual size_t Seek(size_t amount) = 0;
    inline void set_owner(Stream* owner) { owner_ = owner; }

    // If the BufferSource is explicitly marked closed, then it
    // should not accept any more pending data than what's already
    // in it's queue, if any, and it should send EOS as soon as possible.
    // The set_closed state will not be relevant to all sources
    // (e.g. ArrayBufferViewSource and NullSource) so the default
    // implementation is to do nothing.
    virtual void set_closed() { }

    static Source* FromObject(v8::Local<v8::Object> object);

   protected:
    void AttachToObject(v8::Local<v8::Object> object);
    inline Stream* owner() { return owner_; }

   private:
    Stream* owner_;
  };

  Buffer() = default;
  Buffer(const Buffer& other) = delete;
  Buffer(const Buffer&& src) = delete;
  Buffer& operator=(const Buffer& other) = delete;
  Buffer& operator=(const Buffer&& src) = delete;

  // Marks the Buffer as having ended, preventing new Buffer::Chunk
  // instances from being added and allowing the Pull operation to know when
  // to signal that the flow of data is completed.
  inline void End() { ended_ = true; }
  inline bool is_ended() const { return ended_; }

  // Push inbound data onto the buffer.
  void Push(Environment* env, const uint8_t* data, size_t len);

  // Push outbound data onto the buffer.
  void Push(
      std::shared_ptr<v8::BackingStore> data,
      size_t length,
      size_t offset = 0);

  // Increment the given number of bytes within the buffer. If amount
  // is greater than length(), length() bytes are advanced. Returns
  // the actual number of bytes advanced. Will not cause bytes to be
  // freed.
  size_t Seek(size_t amount);

  // Acknowledge the given number of bytes in the buffer. May cause
  // bytes to be freed.
  size_t Acknowledge(size_t amount);

  // Clears any bytes remaining in the buffer.
  inline void Clear() {
    queue_.clear();
    head_ = 0;
    length_ = 0;
    remaining_ = 0;
  }

  // The total number of unacknowledged bytes remaining. The length
  // is incremented by Push and decremented by Acknowledge.
  inline size_t length() const { return length_; }

  // The total number of unread bytes remaining. The remaining
  // length is incremental by Push and decremented by Seek.
  inline size_t remaining() const { return remaining_; }

  // Flushes the entire inbound queue into a v8::Local<v8::Array>
  // of Uint8Array instances, returning the total number of bytes
  // released to the consumer.
  v8::Maybe<size_t> Release(Consumer* consumer);

  int DoPull(
      bob::Next<ngtcp2_vec> next,
      int options,
      ngtcp2_vec* data,
      size_t count,
      size_t max_count_hint) override;

  void MemoryInfo(MemoryTracker* tracker) const override;
  SET_MEMORY_INFO_NAME(Buffer);
  SET_SELF_SIZE(Buffer);

 private:
  Chunk::Queue queue_;
  bool ended_ = false;

  // The queue_ index of the current read head.
  // This is incremented by Seek() as necessary and
  // decremented by Acknowledge() as data is consumed.
  size_t head_ = 0;
  size_t length_ = 0;
  size_t remaining_ = 0;
};

// The JSQuicBufferConsumer receives inbound data for a Stream
// and forwards that up as Uint8Array instances to the JavaScript
// API.
class JSQuicBufferConsumer : public Buffer::Consumer,
                             public AsyncWrap {
 public:
  static void Initialize(Environment* env, v8::Local<v8::Object> target);
  static void New(const v8::FunctionCallbackInfo<v8::Value>& args);

  SET_NO_MEMORY_INFO()
  SET_MEMORY_INFO_NAME(JSQuicBufferConsumer)
  SET_SELF_SIZE(JSQuicBufferConsumer)

  JSQuicBufferConsumer(
      Environment* env,
      v8::Local<v8::Object> wrap);

  v8::Maybe<size_t> Process(
      Buffer::Chunk::Queue queue,
      bool ended = false) override;
};

// The NullSource is used when no payload source is provided
// for a Stream. Whenever DoPull is called, it simply
// immediately responds with no data and EOS set.
class NullSource : public Buffer::Source {
 public:
  NullSource() = default;

  int DoPull(
      bob::Next<ngtcp2_vec> next,
      int options,
      ngtcp2_vec* data,
      size_t count,
      size_t max_count_hint) override;

  BaseObjectPtr<BaseObject> GetStrongPtr() override {
    return BaseObjectPtr<BaseObject>();
  }

  size_t Acknowledge(uint64_t offset, size_t datalen) override {
    return 0;
  }

  size_t Seek(size_t amount) override {
    return 0;
  }

  SET_NO_MEMORY_INFO()
  SET_MEMORY_INFO_NAME(NullSource)
  SET_SELF_SIZE(NullSource)
};

// Receives a single ArrayBufferView and uses it's contents as the
// complete source of outbound data for the Stream.
class ArrayBufferViewSource : public Buffer::Source,
                              public BaseObject {
 public:
  static void Initialize(Environment* env, v8::Local<v8::Object> target);
  static void New(const v8::FunctionCallbackInfo<v8::Value>& args);

  int DoPull(
      bob::Next<ngtcp2_vec> next,
      int options,
      ngtcp2_vec* data,
      size_t count,
      size_t max_count_hint) override;

  BaseObjectPtr<BaseObject> GetStrongPtr() override {
    return BaseObjectPtr<BaseObject>(this);
  }
  size_t Acknowledge(uint64_t offset, size_t datalen) override;
  size_t Seek(size_t amount) override;

  void MemoryInfo(MemoryTracker* tracker) const override;
  SET_MEMORY_INFO_NAME(ArrayBufferViewSource);
  SET_SELF_SIZE(ArrayBufferViewSource);

 private:
  ArrayBufferViewSource(
      Environment* env,
      v8::Local<v8::Object> wrap,
      std::unique_ptr<Buffer::Chunk> chunk);

  std::unique_ptr<Buffer::Chunk> chunk_;
};

// Implements StreamBase to asynchronously accept outbound data from the
// JavaScript side.
class StreamSource : public AsyncWrap,
                     public StreamBase,
                     public Buffer::Source {
 public:
  static void Initialize(Environment* env, v8::Local<v8::Object> target);
  static void New(const v8::FunctionCallbackInfo<v8::Value>& args);

  size_t Acknowledge(uint64_t offset, size_t datalen) override;
  size_t Seek(size_t amount) override;

  // This is a write-only stream. These are ignored.
  int ReadStart() override { return 0; }
  int ReadStop() override { return 0; }
  bool IsAlive() override { return !queue_.is_ended(); }
  bool IsClosing() override { return queue_.is_ended(); }

  int DoShutdown(ShutdownWrap* wrap) override;
  void set_closed() override;

  int DoWrite(
      WriteWrap* w,
      uv_buf_t* bufs,
      size_t count,
      uv_stream_t* send_handle) override;

  AsyncWrap* GetAsyncWrap() override { return this; }

  int DoPull(
      bob::Next<ngtcp2_vec> next,
      int options,
      ngtcp2_vec* data,
      size_t count,
      size_t max_count_hint) override;

  BaseObjectPtr<BaseObject> GetStrongPtr() override {
    return BaseObjectPtr<BaseObject>(this);
  }

  void MemoryInfo(MemoryTracker* tracker) const override;
  SET_MEMORY_INFO_NAME(StreamSource);
  SET_SELF_SIZE(StreamSource);

 private:
  StreamSource(Environment* env, v8::Local<v8::Object> wrap);

  Buffer queue_;
};

// Implements StreamListener to receive data from any native level
// StreamBase implementation.
class StreamBaseSource : public AsyncWrap,
                         public Buffer::Source,
                         public StreamListener {
 public:
  static void Initialize(Environment* env, v8::Local<v8::Object> target);
  static void New(const v8::FunctionCallbackInfo<v8::Value>& args);

  ~StreamBaseSource() override;

  int DoPull(
      bob::Next<ngtcp2_vec> next,
      int options,
      ngtcp2_vec* data,
      size_t count,
      size_t max_count_hint) override;

  size_t Acknowledge(uint64_t offset, size_t datalen) override;
  size_t Seek(size_t amount) override;

  uv_buf_t OnStreamAlloc(size_t suggested_size) override;
  void OnStreamRead(ssize_t nread, const uv_buf_t& buf) override;

  BaseObjectPtr<BaseObject> GetStrongPtr() override {
    return BaseObjectPtr<BaseObject>(this);
  }

  void set_closed() override;

  void MemoryInfo(MemoryTracker* tracker) const override;
  SET_MEMORY_INFO_NAME(StreamBaseSource)
  SET_SELF_SIZE(StreamBaseSource)

 private:
  StreamBaseSource(
      Environment* env,
      v8::Local<v8::Object> wrap,
      StreamResource* resource,
      BaseObjectPtr<AsyncWrap> strong_ptr = BaseObjectPtr<AsyncWrap>());

  StreamResource* resource_;
  BaseObjectPtr<AsyncWrap> strong_ptr_;
  Buffer buffer_;
};
}  // namespace quic
}  // namespace node

#endif  // NODE_WANT_INTERNALS
#endif  // SRC_QUIC_BUFFER_H_
