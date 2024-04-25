// Copyright 2023-2024 DreamWorks Animation LLC
// SPDX-License-Identifier: Apache-2.0

#include "XPURayHandlers.h"

#include "RayHandlerUtils.h"
#include <moonray/rendering/pbr/core/Aov.h>

#include <moonray/rendering/mcrt_common/Clock.h>
#include <moonray/rendering/mcrt_common/ProfileAccumulatorHandles.h>
#include <moonray/rendering/mcrt_common/ThreadLocalState.h>
#include <moonray/rendering/pbr/core/PbrTLState.h>
#include <moonray/rendering/pbr/core/RayState.h>
#include <moonray/rendering/pbr/integrator/PathIntegrator.h>
#include <moonray/rendering/pbr/integrator/PathIntegratorUtil.h>
#include <moonray/rendering/rt/gpu/GPUAccelerator.h>

#include <scene_rdl2/common/math/Color.h>
#include <scene_rdl2/common/math/Vec3.h>
#include <scene_rdl2/scene/rdl2/VisibilityFlags.h>


namespace moonray {
namespace pbr {

// warning #1684: conversion from pointer to
// same-sized integral type (potential portability problem)
#pragma warning push
#pragma warning disable 1684

namespace {

unsigned
computeXPUOcclusionQueriesOnGPU(mcrt_common::ThreadLocalState *tls,
                                unsigned numRays,
                                BundledOcclRay* rays,
                                const rt::GPURay* gpuRays,
                                BundledRadiance *results,
                                std::atomic<int>& threadsUsingGPU)
{
    pbr::TLState *pbrTls = tls->mPbrTls.get();
    scene_rdl2::alloc::Arena *arena = &tls->mArena;

    // Update ray stats.
    pbrTls->mStatistics.addToCounter(STATS_OCCLUSION_RAYS, numRays);
    pbrTls->mStatistics.addToCounter(STATS_BUNDLED_OCCLUSION_RAYS, numRays);
    pbrTls->mStatistics.addToCounter(STATS_BUNDLED_GPU_OCCLUSION_RAYS, numRays);

    const FrameState &fs = *pbrTls->mFs;
    rt::GPUAccelerator *accel = const_cast<rt::GPUAccelerator*>(fs.mGPUAccel);
    const bool disableShadowing = !fs.mIntegrator->getEnableShadowing();

    threadsUsingGPU++;
    {
        EXCL_ACCUMULATOR_PROFILE(pbrTls, EXCL_ACCUM_GPU_OCCLUSION);

        // Call the GPU and wait for it to finish processing these rays.
        accel->occluded(tls->mThreadIdx, numRays, gpuRays, rays, sizeof(rays[0]));
    }
    threadsUsingGPU--;

    unsigned char *isOccluded = accel->getOutputOcclusionBuf(tls->mThreadIdx);

/*
    {
        // SW debug mode for comparison
        unsigned numIncorrect = 0;

        for (size_t i = 0; i < numRays; ++i) {
            const BundledOcclRay &occlRay = raysCopy[i];
            MNRY_ASSERT(occlRay.isValid());

            occlRaysCopy[i] = occlRay;

            mcrt_common::Ray rtRay;

            rtRay.org[0]  = occlRay.mOrigin.x;
            rtRay.org[1]  = occlRay.mOrigin.y;
            rtRay.org[2]  = occlRay.mOrigin.z;
            rtRay.dir[0]  = occlRay.mDir.x;
            rtRay.dir[1]  = occlRay.mDir.y;
            rtRay.dir[2]  = occlRay.mDir.z;
            rtRay.tnear   = occlRay.mMinT;
            rtRay.tfar    = occlRay.mMaxT;
            rtRay.time    = occlRay.mTime;
            rtRay.mask    = scene_rdl2::rdl2::SHADOW;
            rtRay.geomID  = RT_INVALID_RAY_ID;
            rtRay.ext.instance0OrLight = static_cast<BundledOcclRayData *>(
                pbrTls->getListItem(occlRay.mDataPtrHandle, 0))->mLight->getRdlLight();

            //isOccluded[i] = accel->occluded(rtRay);
            if (isOccluded[i] != accel->occluded(rtRay)) {
                 numIncorrect++;
            }
        }

        if (numIncorrect > 0) {
            std::cout << "numIncorrect: " << numIncorrect << std::endl;
        }
    }
*/

    // Create the BundledRadiance objects as required based on the occlusion
    // test results.
    unsigned numRadiancesFilled = 0;
    for (unsigned i = 0; i < numRays; ++i) {
        BundledOcclRay &occlRay = rays[i];

        if (occlRay.mOcclTestType == OcclTestType::FORCE_NOT_OCCLUDED) {
            // See forceSingleRaysUnoccluded()
            scene_rdl2::math::Color tr = getTransmittance(pbrTls, occlRay);
            occlRay.mRadiance = occlRay.mRadiance * tr;
            BundledRadiance *result = &results[numRadiancesFilled++];
            fillBundledRadiance(pbrTls, result, occlRay);

            // LPE
            if (occlRay.mDataPtrHandle != nullHandle) {
                EXCL_ACCUMULATOR_PROFILE(pbrTls, EXCL_ACCUM_AOVS);

                const int numItems = pbrTls->getNumListItems(occlRay.mDataPtrHandle);
                accumLightAovs(pbrTls, occlRay, fs, numItems, tr, nullptr, AovSchema::sLpePrefixNone);
            }

        } else if (!isOccluded[i] || disableShadowing) {
            scene_rdl2::math::Color tr = getTransmittance(pbrTls, occlRay);
            occlRay.mRadiance = occlRay.mRadiance * tr;
            BundledRadiance *result = &results[numRadiancesFilled++];
            fillBundledRadiance(pbrTls, result, occlRay);

            // LPE
            if (occlRay.mDataPtrHandle != nullHandle) {
                EXCL_ACCUMULATOR_PROFILE(pbrTls, EXCL_ACCUM_AOVS);

                const int numItems = pbrTls->getNumListItems(occlRay.mDataPtrHandle);
                accumLightAovs(pbrTls, occlRay, fs, numItems, scene_rdl2::math::sWhite, &tr,
                               AovSchema::sLpePrefixUnoccluded);
                accumVisibilityAovs(pbrTls, occlRay, fs, numItems, reduceTransparency(tr));
            }

        } else {
            // LPE: visibility aovs when we don't hit light
            if (occlRay.mDataPtrHandle != nullHandle) {

                const Light *light = static_cast<BundledOcclRayData *>(
                    pbrTls->getListItem(occlRay.mDataPtrHandle, 0))->mLight;

                // see PathIntegrator::addDirectVisibleLightSampleContributions()
                if (light->getClearRadiusFalloffDistance() != 0.f &&
                    occlRay.mMaxT < light->getClearRadius() + light->getClearRadiusFalloffDistance()) {
                    scene_rdl2::math::Color tr = getTransmittance(pbrTls, occlRay);
                    occlRay.mRadiance = calculateShadowFalloff(light, occlRay.mMaxT, tr * occlRay.mRadiance);
                    BundledRadiance *result = &results[numRadiancesFilled++];
                    fillBundledRadiance(pbrTls, result, occlRay);
                }

                EXCL_ACCUMULATOR_PROFILE(pbrTls, EXCL_ACCUM_AOVS);

                const int numItems = pbrTls->getNumListItems(occlRay.mDataPtrHandle);
                accumVisibilityAovsOccluded(pbrTls, occlRay, fs, numItems);

                // We only accumulate here if we were occluded but we have the flag on. Otherwise
                // it would already have been filled by the previous call.
                if (fs.mAovSchema->hasLpePrefixFlags(AovSchema::sLpePrefixUnoccluded)) {
                    accumLightAovs(pbrTls, occlRay, fs, numItems, scene_rdl2::math::sWhite, nullptr,
                                   AovSchema::sLpePrefixUnoccluded);
                }
            }
        }

        // LPE
        // we are responsible for freeing LPE memory
        if (occlRay.mDataPtrHandle != nullHandle) {
            pbrTls->freeList(occlRay.mDataPtrHandle);
        }
        pbrTls->releaseDeepData(occlRay.mDeepDataHandle);
    }

    return numRadiancesFilled;
}

}


void
xpuRayBundleHandler(mcrt_common::ThreadLocalState *tls,
                    unsigned numRayStates,
                    RayState **rayStates,
                    const rt::GPURay *gpuRays,
                    tbb::spin_mutex& mutex)
{
    // TODO
}

void
xpuOcclusionQueryBundleHandler(mcrt_common::ThreadLocalState *tls,
                               unsigned numRays,
                               BundledOcclRay *rays,
                               const rt::GPURay *gpuRays,
                               std::atomic<int>& threadsUsingGPU)
{
    pbr::TLState *pbrTls = tls->mPbrTls.get();

    EXCL_ACCUMULATOR_PROFILE(pbrTls, EXCL_ACCUM_OCCL_QUERY_HANDLER);

    scene_rdl2::alloc::Arena *arena = &tls->mArena;
    SCOPED_MEM(arena);

    BundledRadiance *results = arena->allocArray<BundledRadiance>(numRays, CACHE_LINE_SIZE);

    unsigned numRadiancesFilled =
        computeXPUOcclusionQueriesOnGPU(tls,
                                        numRays,
                                        rays,
                                        gpuRays,
                                        results,
                                        threadsUsingGPU);

    MNRY_ASSERT(numRadiancesFilled <= numRays);

    CHECK_CANCELLATION(pbrTls, return);

    pbrTls->addRadianceQueueEntries(numRadiancesFilled, results);
}

#pragma warning pop

} // namespace pbr
} // namespace moonray
