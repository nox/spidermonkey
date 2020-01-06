/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/SharedArrayObject.h"

#include "mozilla/Atomics.h"
#include "mozilla/CheckedInt.h"

#include "jsfriendapi.h"

#include "gc/FreeOp.h"
#include "jit/AtomicOperations.h"
#include "js/PropertySpec.h"
#include "js/SharedArrayBuffer.h"
#include "js/Wrapper.h"
#include "util/Memory.h"
#include "vm/SharedMem.h"
#include "wasm/WasmSignalHandlers.h"
#include "wasm/WasmTypes.h"

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using mozilla::CheckedInt;
using mozilla::Maybe;
using mozilla::Nothing;

using namespace js;

static uint32_t SharedArrayAccessibleSize(uint32_t length) {
  return AlignBytes(length, gc::SystemPageSize());
}

// `maxSize` must be something for wasm, nothing for other cases.
SharedArrayRawBuffer* SharedArrayRawBuffer::Allocate(
    uint32_t length, const Maybe<uint32_t>& maxSize,
    const Maybe<size_t>& mappedSize) {
  MOZ_RELEASE_ASSERT(length <= ArrayBufferObject::MaxBufferByteLength);

  uint32_t accessibleSize = SharedArrayAccessibleSize(length);
  if (accessibleSize < length) {
    return nullptr;
  }

  bool preparedForWasm = maxSize.isSome();
  uint32_t computedMaxSize;
  size_t computedMappedSize;

  if (preparedForWasm) {
    computedMaxSize = *maxSize;
    computedMappedSize = mappedSize.isSome()
                             ? *mappedSize
                             : wasm::ComputeMappedSize(computedMaxSize);
  } else {
    computedMappedSize = accessibleSize;
    computedMaxSize = accessibleSize;
  }

  MOZ_ASSERT(accessibleSize <= computedMaxSize);
  MOZ_ASSERT(accessibleSize <= computedMappedSize);

  uint64_t mappedSizeWithHeader = computedMappedSize + gc::SystemPageSize();
  uint64_t accessibleSizeWithHeader = accessibleSize + gc::SystemPageSize();

  void* p = MapBufferMemory(mappedSizeWithHeader, accessibleSizeWithHeader);
  if (!p) {
    return nullptr;
  }

  uint8_t* buffer = reinterpret_cast<uint8_t*>(p) + gc::SystemPageSize();
  uint8_t* base = buffer - sizeof(SharedArrayRawBuffer);
  SharedArrayRawBuffer* rawbuf = new (base) SharedArrayRawBuffer(
      buffer, length, computedMaxSize, computedMappedSize, preparedForWasm);
  MOZ_ASSERT(rawbuf->length_ == length);  // Deallocation needs this
  return rawbuf;
}

void SharedArrayRawBuffer::tryGrowMaxSizeInPlace(uint32_t deltaMaxSize) {
  CheckedInt<uint32_t> newMaxSize = maxSize_;
  newMaxSize += deltaMaxSize;
  MOZ_ASSERT(newMaxSize.isValid());
  MOZ_ASSERT(newMaxSize.value() % wasm::PageSize == 0);

  size_t newMappedSize = wasm::ComputeMappedSize(newMaxSize.value());
  MOZ_ASSERT(mappedSize_ <= newMappedSize);
  if (mappedSize_ == newMappedSize) {
    return;
  }

  if (!ExtendBufferMapping(basePointer(), mappedSize_, newMappedSize)) {
    return;
  }

  mappedSize_ = newMappedSize;
  maxSize_ = newMaxSize.value();
}

bool SharedArrayRawBuffer::wasmGrowToSizeInPlace(const Lock&,
                                                 uint32_t newLength) {
  if (newLength > ArrayBufferObject::MaxBufferByteLength) {
    return false;
  }

  MOZ_ASSERT(newLength >= length_);

  if (newLength == length_) {
    return true;
  }

  uint32_t delta = newLength - length_;
  MOZ_ASSERT(delta % wasm::PageSize == 0);

  uint8_t* dataEnd = dataPointerShared().unwrap(/* for resize */) + length_;
  MOZ_ASSERT(uintptr_t(dataEnd) % gc::SystemPageSize() == 0);

  if (!CommitBufferMemory(dataEnd, delta)) {
    return false;
  }

  // We rely on CommitBufferMemory (and therefore memmap/VirtualAlloc) to only
  // return once it has committed memory for all threads. We only update with a
  // new length once this has occurred.
  length_ = newLength;

  return true;
}

bool SharedArrayRawBuffer::addReference() {
  MOZ_RELEASE_ASSERT(refcount_ > 0);

  // Be careful never to overflow the refcount field.
  for (;;) {
    uint32_t old_refcount = refcount_;
    uint32_t new_refcount = old_refcount + 1;
    if (new_refcount == 0) {
      return false;
    }
    if (refcount_.compareExchange(old_refcount, new_refcount)) {
      return true;
    }
  }
}

void SharedArrayRawBuffer::dropReference() {
  // Normally if the refcount is zero then the memory will have been unmapped
  // and this test may just crash, but if the memory has been retained for any
  // reason we will catch the underflow here.
  MOZ_RELEASE_ASSERT(refcount_ > 0);

  // Drop the reference to the buffer.
  uint32_t new_refcount = --refcount_;  // Atomic.
  if (new_refcount) {
    return;
  }

  size_t mappedSizeWithHeader = mappedSize_ + gc::SystemPageSize();

  // This was the final reference, so release the buffer.
  UnmapBufferMemory(basePointer(), mappedSizeWithHeader);
}

MOZ_ALWAYS_INLINE bool SharedArrayBufferObject::byteLengthGetterImpl(
    JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsSharedArrayBuffer(args.thisv()));
  args.rval().setInt32(
      args.thisv().toObject().as<SharedArrayBufferObject>().byteLength());
  return true;
}

bool SharedArrayBufferObject::byteLengthGetter(JSContext* cx, unsigned argc,
                                               Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsSharedArrayBuffer, byteLengthGetterImpl>(cx,
                                                                         args);
}

// ES2017 draft rev 6390c2f1b34b309895d31d8c0512eac8660a0210
// 24.2.2.1 SharedArrayBuffer( length )
bool SharedArrayBufferObject::class_constructor(JSContext* cx, unsigned argc,
                                                Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, "SharedArrayBuffer")) {
    return false;
  }

  // Step 2.
  uint64_t byteLength;
  if (!ToIndex(cx, args.get(0), &byteLength)) {
    return false;
  }

  // Step 3 (Inlined 24.2.1.1 AllocateSharedArrayBuffer).
  // 24.2.1.1, step 1 (Inlined 9.1.14 OrdinaryCreateFromConstructor).
  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_SharedArrayBuffer,
                                          &proto)) {
    return false;
  }

  // 24.2.1.1, step 3 (Inlined 6.2.7.2 CreateSharedByteDataBlock, step 2).
  // Refuse to allocate too large buffers, currently limited to ~2 GiB.
  if (byteLength > INT32_MAX) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_SHARED_ARRAY_BAD_LENGTH);
    return false;
  }

  // 24.2.1.1, steps 1 and 4-6.
  JSObject* bufobj = New(cx, uint32_t(byteLength), proto);
  if (!bufobj) {
    return false;
  }
  args.rval().setObject(*bufobj);
  return true;
}

SharedArrayBufferObject* SharedArrayBufferObject::New(JSContext* cx,
                                                      uint32_t length,
                                                      HandleObject proto) {
  SharedArrayRawBuffer* buffer =
      SharedArrayRawBuffer::Allocate(length, Nothing(), Nothing());
  if (!buffer) {
    return nullptr;
  }

  SharedArrayBufferObject* obj = New(cx, buffer, length, proto);
  if (!obj) {
    buffer->dropReference();
    return nullptr;
  }

  return obj;
}

SharedArrayBufferObject* SharedArrayBufferObject::New(
    JSContext* cx, SharedArrayRawBuffer* buffer, uint32_t length,
    HandleObject proto) {
  MOZ_ASSERT(cx->realm()->creationOptions().getSharedMemoryAndAtomicsEnabled());

  AutoSetNewObjectMetadata metadata(cx);
  Rooted<SharedArrayBufferObject*> obj(
      cx, NewObjectWithClassProto<SharedArrayBufferObject>(cx, proto));
  if (!obj) {
    return nullptr;
  }

  MOZ_ASSERT(obj->getClass() == &class_);

  cx->runtime()->incSABCount();

  if (!obj->acceptRawBuffer(buffer, length)) {
    return nullptr;
  }

  return obj;
}

bool SharedArrayBufferObject::acceptRawBuffer(SharedArrayRawBuffer* buffer,
                                              uint32_t length) {
  if (!zone()->addSharedMemory(buffer, length,
                               MemoryUse::SharedArrayRawBuffer)) {
    return false;
  }

  setReservedSlot(RAWBUF_SLOT, PrivateValue(buffer));
  setReservedSlot(LENGTH_SLOT, PrivateUint32Value(length));
  return true;
}

void SharedArrayBufferObject::dropRawBuffer() {
  zoneFromAnyThread()->removeSharedMemory(rawBufferObject(), byteLength(),
                                          MemoryUse::SharedArrayRawBuffer);
  setReservedSlot(RAWBUF_SLOT, UndefinedValue());
}

SharedArrayRawBuffer* SharedArrayBufferObject::rawBufferObject() const {
  Value v = getReservedSlot(RAWBUF_SLOT);
  MOZ_ASSERT(!v.isUndefined());
  return reinterpret_cast<SharedArrayRawBuffer*>(v.toPrivate());
}

void SharedArrayBufferObject::Finalize(JSFreeOp* fop, JSObject* obj) {
  // Must be foreground finalizable so that we can account for the object.
  MOZ_ASSERT(fop->onMainThread());
  fop->runtime()->decSABCount();

  SharedArrayBufferObject& buf = obj->as<SharedArrayBufferObject>();

  // Detect the case of failure during SharedArrayBufferObject creation,
  // which causes a SharedArrayRawBuffer to never be attached.
  Value v = buf.getReservedSlot(RAWBUF_SLOT);
  if (!v.isUndefined()) {
    buf.rawBufferObject()->dropReference();
    buf.dropRawBuffer();
  }
}

/* static */
void SharedArrayBufferObject::addSizeOfExcludingThis(
    JSObject* obj, mozilla::MallocSizeOf mallocSizeOf, JS::ClassInfo* info) {
  // Divide the buffer size by the refcount to get the fraction of the buffer
  // owned by this thread. It's conceivable that the refcount might change in
  // the middle of memory reporting, in which case the amount reported for
  // some threads might be to high (if the refcount goes up) or too low (if
  // the refcount goes down). But that's unlikely and hard to avoid, so we
  // just live with the risk.
  const SharedArrayBufferObject& buf = obj->as<SharedArrayBufferObject>();
  info->objectsNonHeapElementsShared +=
      buf.byteLength() / buf.rawBufferObject()->refcount();
}

/* static */
void SharedArrayBufferObject::copyData(
    Handle<SharedArrayBufferObject*> toBuffer, uint32_t toIndex,
    Handle<SharedArrayBufferObject*> fromBuffer, uint32_t fromIndex,
    uint32_t count) {
  MOZ_ASSERT(toBuffer->byteLength() >= count);
  MOZ_ASSERT(toBuffer->byteLength() >= toIndex + count);
  MOZ_ASSERT(fromBuffer->byteLength() >= fromIndex);
  MOZ_ASSERT(fromBuffer->byteLength() >= fromIndex + count);

  jit::AtomicOperations::memcpySafeWhenRacy(
      toBuffer->dataPointerShared() + toIndex,
      fromBuffer->dataPointerShared() + fromIndex, count);
}

SharedArrayBufferObject* SharedArrayBufferObject::createFromNewRawBuffer(
    JSContext* cx, SharedArrayRawBuffer* buffer, uint32_t initialSize) {
  MOZ_ASSERT(cx->realm()->creationOptions().getSharedMemoryAndAtomicsEnabled());

  AutoSetNewObjectMetadata metadata(cx);
  SharedArrayBufferObject* obj =
      NewBuiltinClassInstance<SharedArrayBufferObject>(cx);
  if (!obj) {
    buffer->dropReference();
    return nullptr;
  }

  cx->runtime()->incSABCount();

  if (!obj->acceptRawBuffer(buffer, initialSize)) {
    buffer->dropReference();
    return nullptr;
  }

  return obj;
}

static const JSClassOps SharedArrayBufferObjectClassOps = {
    nullptr,                            // addProperty
    nullptr,                            // delProperty
    nullptr,                            // enumerate
    nullptr,                            // newEnumerate
    nullptr,                            // resolve
    nullptr,                            // mayResolve
    SharedArrayBufferObject::Finalize,  // finalize
    nullptr,                            // call
    nullptr,                            // hasInstance
    nullptr,                            // construct
    nullptr,                            // trace
};

static const JSFunctionSpec sharedarrray_functions[] = {JS_FS_END};

static const JSPropertySpec sharedarrray_properties[] = {
    JS_SELF_HOSTED_SYM_GET(species, "$SharedArrayBufferSpecies", 0), JS_PS_END};

static const JSFunctionSpec sharedarray_proto_functions[] = {
    JS_SELF_HOSTED_FN("slice", "SharedArrayBufferSlice", 2, 0), JS_FS_END};

static const JSPropertySpec sharedarray_proto_properties[] = {
    JS_PSG("byteLength", SharedArrayBufferObject::byteLengthGetter, 0),
    JS_STRING_SYM_PS(toStringTag, "SharedArrayBuffer", JSPROP_READONLY),
    JS_PS_END};

static const ClassSpec SharedArrayBufferObjectClassSpec = {
    GenericCreateConstructor<SharedArrayBufferObject::class_constructor, 1,
                             gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<SharedArrayBufferObject>,
    sharedarrray_functions,
    sharedarrray_properties,
    sharedarray_proto_functions,
    sharedarray_proto_properties};

const JSClass SharedArrayBufferObject::class_ = {
    "SharedArrayBuffer",
    JSCLASS_DELAY_METADATA_BUILDER |
        JSCLASS_HAS_RESERVED_SLOTS(SharedArrayBufferObject::RESERVED_SLOTS) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_SharedArrayBuffer) |
        JSCLASS_FOREGROUND_FINALIZE,
    &SharedArrayBufferObjectClassOps, &SharedArrayBufferObjectClassSpec,
    JS_NULL_CLASS_EXT};

const JSClass SharedArrayBufferObject::protoClass_ = {
    "SharedArrayBufferPrototype",
    JSCLASS_HAS_CACHED_PROTO(JSProto_SharedArrayBuffer), JS_NULL_CLASS_OPS,
    &SharedArrayBufferObjectClassSpec};

bool js::IsSharedArrayBuffer(HandleValue v) {
  return v.isObject() && v.toObject().is<SharedArrayBufferObject>();
}

bool js::IsSharedArrayBuffer(HandleObject o) {
  return o->is<SharedArrayBufferObject>();
}

bool js::IsSharedArrayBuffer(JSObject* o) {
  return o->is<SharedArrayBufferObject>();
}

SharedArrayBufferObject& js::AsSharedArrayBuffer(HandleObject obj) {
  MOZ_ASSERT(IsSharedArrayBuffer(obj));
  return obj->as<SharedArrayBufferObject>();
}

JS_FRIEND_API uint32_t JS::GetSharedArrayBufferByteLength(JSObject* obj) {
  auto* aobj = obj->maybeUnwrapAs<SharedArrayBufferObject>();
  return aobj ? aobj->byteLength() : 0;
}

JS_FRIEND_API void JS::GetSharedArrayBufferLengthAndData(JSObject* obj,
                                                         uint32_t* length,
                                                         bool* isSharedMemory,
                                                         uint8_t** data) {
  MOZ_ASSERT(obj->is<SharedArrayBufferObject>());
  *length = obj->as<SharedArrayBufferObject>().byteLength();
  *data = obj->as<SharedArrayBufferObject>().dataPointerShared().unwrap(
      /*safe - caller knows*/);
  *isSharedMemory = true;
}

JS_FRIEND_API JSObject* JS::NewSharedArrayBuffer(JSContext* cx,
                                                 uint32_t nbytes) {
  MOZ_ASSERT(cx->realm()->creationOptions().getSharedMemoryAndAtomicsEnabled());

  MOZ_ASSERT(nbytes <= INT32_MAX);
  return SharedArrayBufferObject::New(cx, nbytes, /* proto = */ nullptr);
}

JS_FRIEND_API bool JS::IsSharedArrayBufferObject(JSObject* obj) {
  return obj->canUnwrapAs<SharedArrayBufferObject>();
}

JS_FRIEND_API uint8_t* JS::GetSharedArrayBufferData(
    JSObject* obj, bool* isSharedMemory, const JS::AutoRequireNoGC&) {
  auto* aobj = obj->maybeUnwrapAs<SharedArrayBufferObject>();
  if (!aobj) {
    return nullptr;
  }
  *isSharedMemory = true;
  return aobj->dataPointerShared().unwrap(/*safe - caller knows*/);
}

JS_PUBLIC_API bool JS::ContainsSharedArrayBuffer(JSContext* cx) {
  return cx->runtime()->hasLiveSABs();
}
