/**
 * @file llvkaddonlock.h
 * @brief The DLSS add-on's lock (#1517). Frame generation's present thread evaluates inside the add-on
 *        (LLVKNeuralRecon::fgEvaluate) while the main thread may create or release a feature there (a DLSS
 *        quality change: LLCleanDlssRRVK; the generator's own feature: LLVKFrameGen::prepareFeature). A device
 *        wait used to keep the two apart as a side effect -- it drained the present thread; it no longer does.
 *        This lock does, and is held only around the add-on call itself.
 *
 *          wait()    the present thread's evaluate: waits. Only a call of the main thread into the add-on can
 *                    be ahead of it, and that thread is then not waiting for the present thread.
 *          tryFor()  the main thread's create / release: at most a bound. An evaluate that does not return (the
 *                    generator is one of the calls a present thread can be held in) costs a refused create or
 *                    a feature kept until exit -- never a main thread waiting without a bound.
 *
 *        NO LOCK ORDER with the present thread's handoff (vk_present_handoff.h): nothing here knows it, its
 *        mutex is never held across a job (the job runs unlocked) and never taken inside the add-on, and the
 *        main thread never holds this lock while it waits for the present thread (a slot, a drain, a stop):
 *        it is taken and let go inside one LLVKNeuralRecon call. So no thread ever holds both.
 *        The per-frame DLSS evaluate (main thread) takes no lock: it runs beside the frame generation
 *        evaluate by design (two features, the add-on's failure text is per thread) and is on the same thread
 *        as every create / release.
 *
 *        Pure C++: built and tested on the host by scripts/check/test_framegen_present.cpp (ThreadSanitizer too).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef LL_LLVKADDONLOCK_H
#define LL_LLVKADDONLOCK_H

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

class LLVKAddonLock
{
public:
    // The main thread's bound: far above an evaluate that returns (it records a few commands).
    static constexpr uint32_t kBoundMs = 250;
    // The lock, or (false) not: the bound passed. Let go when it goes out of scope.
    class Hold
    {
    public:
        Hold(Hold&& o) noexcept : mOwner(o.mOwner) { o.mOwner = nullptr; }
        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;
        ~Hold() { if (mOwner) mOwner->release(); }
        explicit operator bool() const { return mOwner != nullptr; }
        bool operator!() const { return mOwner == nullptr; }
    private:
        friend class LLVKAddonLock;
        explicit Hold(LLVKAddonLock* owner) : mOwner(owner) {}
        LLVKAddonLock* mOwner;
    };

    Hold wait()
    {
        std::unique_lock<std::mutex> lock(mMutex);
        mFree.wait(lock, [this] { return !mHeld; });
        mHeld = true;
        return Hold(this);
    }
    Hold tryFor(std::chrono::milliseconds bound = std::chrono::milliseconds(kBoundMs))
    {
        std::unique_lock<std::mutex> lock(mMutex);
        if (!mFree.wait_for(lock, bound, [this] { return !mHeld; })) return Hold(nullptr);
        mHeld = true;
        return Hold(this);
    }

private:
    void release()
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mHeld = false;
        }
        mFree.notify_one();
    }

    std::mutex mMutex;                  // guards mHeld only: never held across an add-on call
    std::condition_variable mFree;
    bool mHeld = false;                 // a thread is inside the add-on under this lock
};

#endif // LL_LLVKADDONLOCK_H
