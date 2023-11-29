// Copyright 2023 DreamWorks Animation LLC
// SPDX-License-Identifier: Apache-2.0

//
//
#include "RayHandlerUtils.h"
#include "RayHandlers.h"

#include <moonray/rendering/pbr/core/Aov.h>

#include <moonray/rendering/geom/prim/Primitive.h>
#include <moonray/rendering/mcrt_common/Clock.h>
#include <moonray/rendering/mcrt_common/ProfileAccumulatorHandles.h>
#include <moonray/rendering/mcrt_common/ThreadLocalState.h>
#include <moonray/rendering/pbr/core/PbrTLState.h>
#include <moonray/rendering/pbr/core/RayState.h>
#include <moonray/rendering/pbr/core/Scene.h>
#include <moonray/rendering/pbr/integrator/PathIntegrator.h>
#include <moonray/rendering/pbr/integrator/PathIntegratorUtil.h>
#include <moonray/rendering/pbr/integrator/VolumeTransmittance.h>
#include <moonray/rendering/rt/EmbreeAccelerator.h>
#include <moonray/rendering/shading/Material.h>
#include <moonray/rendering/shading/Types.h>

#include <scene_rdl2/common/math/Color.h>
#include <scene_rdl2/common/math/Vec3.h>
#include <scene_rdl2/scene/rdl2/VisibilityFlags.h>

#define RAY_HANDLER_STD_SORT_CUTOFF     200

namespace moonray {
namespace pbr {

// warning #1684: conversion from pointer to
// same-sized integral type (potential portability problem)
#pragma warning push
#pragma warning disable 1684

//-----------------------------------------------------------------------------

// Returns the number of BundledRadiance entries filled in.
unsigned
areSingleRaysOccluded(pbr::TLState *pbrTls, unsigned numEntries, BundledOcclRay **entries,
                      BundledRadiance *results, RayHandlerFlags flags)
{
    const FrameState &fs = *pbrTls->mFs;
    const rt::EmbreeAccelerator *accel = fs.mEmbreeAccel;
    const bool disableShadowing = !fs.mIntegrator->getEnableShadowing();
    unsigned numRadiancesFilled = 0;

    for (unsigned i = 0; i < numEntries; ++i) {
        BundledOcclRay &occlRay = *entries[i];

        MNRY_ASSERT(occlRay.isValid());

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
        rtRay.ext.shadowReceiverId = occlRay.mShadowReceiverId;
        rtRay.ext.volumeInstanceState = 0;  // Here we piggyback on this data member (which isn't used in occusion rays)
                                            // to signal to the skipOcclusionFilter Embree intersection filter that the
                                            // ray originated from a regular surface rather than from a volume. We can
                                            // be sure of this because a scene with volumes will trigger a fallback
                                            // to scalar mode, so there won't be any vector-mode occlusion rays
                                            // generated by volumes.

        bool isOccluded;
        {
            MNRY_ASSERT(occlRay.mOcclTestType == OcclTestType::STANDARD);
            EXCL_ACCUMULATOR_PROFILE(pbrTls, EXCL_ACCUM_EMBREE_OCCLUSION);
            isOccluded = accel->occluded(rtRay);
        }

        if (!isOccluded || disableShadowing) {
            // At this point, we know that the ray is not occluded, but we still need to
            // apply volume transmittance to the final radiance value.
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
        pbrTls->releaseCryptomatteData(occlRay.mCryptomatteDataHandle);
    }

    return numRadiancesFilled;
}

// Returns the number of BundledRadiance entries filled in.
unsigned
forceSingleRaysUnoccluded(pbr::TLState *pbrTls, unsigned numEntries, BundledOcclRay **entries,
                          BundledRadiance *results, RayHandlerFlags flags)
{
    const FrameState &fs = *pbrTls->mFs;

    for (unsigned i = 0; i < numEntries; ++i) {
        BundledOcclRay &occlRay = *entries[i];

        MNRY_ASSERT(occlRay.isValid());
        MNRY_ASSERT(occlRay.mOcclTestType == OcclTestType::FORCE_NOT_OCCLUDED);

        // At this point, we know that the ray is not occluded, but we still need to
        // apply volume transmittance to the final radiance value.
        scene_rdl2::math::Color tr = getTransmittance(pbrTls, occlRay);
        occlRay.mRadiance = occlRay.mRadiance * tr;

        BundledRadiance *result = &results[i];
        fillBundledRadiance(pbrTls, result, occlRay);

        // LPE
        if (occlRay.mDataPtrHandle != nullHandle) {
            EXCL_ACCUMULATOR_PROFILE(pbrTls, EXCL_ACCUM_AOVS);

            const int numItems = pbrTls->getNumListItems(occlRay.mDataPtrHandle);
            accumLightAovs(pbrTls, occlRay, fs, numItems, tr, nullptr, AovSchema::sLpePrefixNone);
        }

        // LPE
        // we are responsible for freeing LPE memory
        if (occlRay.mDataPtrHandle != nullHandle) {
            pbrTls->freeList(occlRay.mDataPtrHandle);
        }
        pbrTls->releaseDeepData(occlRay.mDeepDataHandle);
        pbrTls->releaseCryptomatteData(occlRay.mCryptomatteDataHandle);
    }

    return numEntries;
}

// Perform all occlusion checks. All the heavy lifting is pretty much done in this loop!
// Returns the number of BundledRadiance entries filled in.
unsigned
computeOcclusionQueriesBundled(pbr::TLState *pbrTls, unsigned numEntries,
                               BundledOcclRay **entries, BundledRadiance *results,
                               RayHandlerFlags flags)
{
    // Update ray stats.
    pbrTls->mStatistics.addToCounter(STATS_OCCLUSION_RAYS, numEntries);
    pbrTls->mStatistics.addToCounter(STATS_BUNDLED_OCCLUSION_RAYS, numEntries);

    unsigned totalRadiancesFilled = 0;

    // Sort no-op rays to come after standard rays. This allows us to process all standard rays together
    // and skip the occlusion test on all no-op rays.
    BundledOcclRay **noOpEntries = std::partition(entries, entries + numEntries,
        [](BundledOcclRay *r){return r->mOcclTestType == OcclTestType::STANDARD;});

    // Exclude all the no-op entries so we don't run occlusion tests for them
    unsigned numStandardEntries = noOpEntries - entries;
    unsigned numNoOpEntries = numEntries - numStandardEntries;
    numEntries = numStandardEntries;

    if (numEntries) {
        unsigned numRadiancesFilled = areSingleRaysOccluded(pbrTls, numEntries, entries, results, flags);
        results += numRadiancesFilled;
        totalRadiancesFilled += numRadiancesFilled;
    }

    // Handle no-op rays.
    if (numNoOpEntries) {
        totalRadiancesFilled += forceSingleRaysUnoccluded(pbrTls, numNoOpEntries, noOpEntries, results, flags);
    }

    return totalRadiancesFilled;
}

// Perform all presence shadows checks. All the heavy lifting is pretty much done in this loop!
// Returns the number of BundledRadiance entries filled in.
unsigned
computePresenceShadowsQueriesBundled(pbr::TLState *pbrTls, unsigned int numEntries,
                                     BundledOcclRay **entries, BundledRadiance *results,
                                     RayHandlerFlags flags)
{
    // Presence handling code for direct lighting
    if (numEntries == 0) {
        return 0;
    }
    const FrameState &fs = *pbrTls->mFs;
    const bool disableShadowing = !fs.mIntegrator->getEnableShadowing();
    unsigned int numRadiancesFilled = 0;
    for (unsigned int i = 0; i < numEntries; ++i) {
        BundledOcclRay &occlRay = *entries[i];
        MNRY_ASSERT(occlRay.isValid());
        // we always have the data block here
        const BundledOcclRayData *b = static_cast<BundledOcclRayData *>(
            pbrTls->getListItem(occlRay.mDataPtrHandle, 0));

        mcrt_common::Ray shadowRay(occlRay.mOrigin, occlRay.mDir,
            occlRay.mMinT, occlRay.mMaxT, occlRay.mTime, occlRay.mDepth);

        float presence = 0.0f;
        accumulateRayPresence(pbrTls,
                              b->mLight,
                              shadowRay,
                              b->mRayEpsilon,
                              fs.mMaxPresenceDepth,
                              presence);

        // At this point, we know that the ray is not occluded, but we still need to
        // apply volume transmittance to the final radiance value.
        scene_rdl2::math::Color tr = getTransmittance(pbrTls, occlRay);
        occlRay.mRadiance = occlRay.mRadiance * tr;

        if (scene_rdl2::math::isEqual(presence, 0.0f) || disableShadowing) {
            BundledRadiance *result = &results[numRadiancesFilled++];
            fillBundledRadiance(pbrTls, result, occlRay);
        } else {
            // Presence value indicates light is partially blocked.
            // Scale radiance by (1 - presence)
            BundledRadiance *result = &results[numRadiancesFilled++];
            fillBundledRadiance(pbrTls, result, occlRay);
            result->mRadiance[0] *= (1 - presence);
            result->mRadiance[1] *= (1 - presence);
            result->mRadiance[2] *= (1 - presence);
        }

        // LPE
        {
            EXCL_ACCUMULATOR_PROFILE(pbrTls, EXCL_ACCUM_AOVS);

            const int numItems = pbrTls->getNumListItems(occlRay.mDataPtrHandle);

            // If we are rendering with unoccluded flags we want to ignore presence values when accumulating
            // them to the aov:
            const scene_rdl2::math::Color occlusionValue = (1.f - presence) * tr;
            accumLightAovs(pbrTls, occlRay, fs, numItems, scene_rdl2::math::sWhite, &occlusionValue,
                           AovSchema::sLpePrefixUnoccluded);

            accumVisibilityAovs(pbrTls, occlRay, fs, numItems, reduceTransparency(occlusionValue));
        }

        // we are responsible for freeing data memory
        pbrTls->freeList(occlRay.mDataPtrHandle);
        pbrTls->releaseCryptomatteData(occlRay.mCryptomatteDataHandle);
    }
    return numRadiancesFilled;
}

//-----------------------------------------------------------------------------

void
rayBundleHandler(mcrt_common::ThreadLocalState *tls, unsigned numEntries,
                 WrappedRayState *wrappedRayStates, void *userData)
{
    pbr::TLState *pbrTls = tls->mPbrTls.get();

    EXCL_ACCUMULATOR_PROFILE(pbrTls, EXCL_ACCUM_RAY_HANDLER);

    MNRY_ASSERT(numEntries);

    RayState **rayStates = &wrappedRayStates[0].mRsPtr;

    // By convention, if userData is null then rayState contains an array of raw
    // RayState pointers.
    RayHandlerFlags handlerFlags = RayHandlerFlags((uint64_t)userData);

    const FrameState &fs = *pbrTls->mFs;

    scene_rdl2::alloc::Arena *arena = &tls->mArena;
    SCOPED_MEM(arena);

    // heat map
    int64_t ticks = 0;
    MCRT_COMMON_CLOCK_OPEN(fs.mRequiresHeatMap? &ticks : nullptr);

    // Perform all intersection checks.
    if (numEntries) {
        pbrTls->mStatistics.addToCounter(STATS_INTERSECTION_RAYS, numEntries);
        pbrTls->mStatistics.addToCounter(STATS_BUNDLED_INTERSECTION_RAYS, numEntries);
        EXCL_ACCUMULATOR_PROFILE(pbrTls, EXCL_ACCUM_EMBREE_INTERSECTION);

        const rt::EmbreeAccelerator *accel = fs.mEmbreeAccel;
        for (unsigned i = 0; i < numEntries; ++i) {
            RayState *rs = rayStates[i];
            MNRY_ASSERT(isValid(rs));
            accel->intersect(rs->mRay);
        }
    }

    // Volumes - compute volume radiance and transmission for each ray
    for (unsigned i = 0; i < numEntries; ++i) {
        RayState &rs = *rayStates[i];
        const mcrt_common::Ray &ray = rs.mRay;
        const Subpixel &sp = rs.mSubpixel;
        PathVertex &pv = rs.mPathVertex;
        const int lobeType = pv.nonMirrorDepth == 0 ? 0 : pv.lobeType;
        const unsigned sequenceID = rs.mSequenceID;
        float *aovs = nullptr;
        PathIntegrator::DeepParams *deepParams = nullptr; // TODO: MOONRAY-3133 support deep output of volumes
        rs.mVolRad = scene_rdl2::math::sBlack;
        VolumeTransmittance vt;
        vt.reset();
        float volumeSurfaceT = scene_rdl2::math::sMaxValue;
        rs.mVolHit = fs.mIntegrator->computeRadianceVolume(pbrTls, ray, sp, pv, lobeType,
            rs.mVolRad, sequenceID, vt, aovs, deepParams, &rs, &volumeSurfaceT);
        rs.mVolTr = vt.mTransmittanceE;
        rs.mVolTh = vt.mTransmittanceH;
        rs.mVolTalpha = vt.mTransmittanceAlpha;
        rs.mVolTm = vt.mTransmittanceMin;
        rs.mVolumeSurfaceT = volumeSurfaceT;
    }

    CHECK_CANCELLATION(pbrTls, return);

    // heat maps
    MCRT_COMMON_CLOCK_CLOSE();
    if (fs.mRequiresHeatMap) {
        heatMapBundledUpdate(pbrTls, ticks, rayStates, numEntries);
    }

    //
    // Sort by material to minimize locks when adding to shared shade queues.
    //
    struct SortedEntry
    {
        uint32_t mSortKey;                      // Material bundled id is stored in here.
        uint32_t mRsIdx;                        // Global ray state index.
        const shading::Material *mMaterial;
    };
    SortedEntry *sortedEntries = arena->allocArray<SortedEntry>(numEntries, CACHE_LINE_SIZE);
    unsigned numSortedEntries = 0;
    uint32_t maxSortKey = 0;

    // Allocate memory to gather raystates so we can bulk free them later in the function.
    unsigned numRayStatesToFree = 0;
    RayState **rayStatesToFree = arena->allocArray<RayState *>(numEntries);

    RayState *baseRayState = indexToRayState(0);
    const scene_rdl2::rdl2::Layer *layer = fs.mLayer;

    for (unsigned i = 0; i < numEntries; ++i) {

        SortedEntry &sortedEntry = sortedEntries[numSortedEntries];
        mcrt_common::Ray &ray = rayStates[i]->mRay;
        PathVertex &pv = rayStates[i]->mPathVertex;

        if (ray.geomID == -1) {
            // We didn't hit anything.
            sortedEntry.mSortKey = 0;
            sortedEntry.mRsIdx = rayStates[i] - baseRayState;
            sortedEntry.mMaterial = nullptr;
            ++numSortedEntries;

            // Prevent aliasing in the visibility aov by accounting for 
            // primary rays that don't hit anything 
            if (ray.getDepth() == 0) {
                const AovSchema &aovSchema = *fs.mAovSchema;

                // If we're on the edge of the geometry, some rays should count as "hits", some as "misses". Here, 
                // we're adding light_sample_count * lights number of "misses" to the visibility aov to account for 
                // the light samples that couldn't be taken because the primary ray doesn't hit anything. 
                // This improves aliasing on the edges.
                if (!aovSchema.empty()) {
                    const LightAovs &lightAovs = *fs.mLightAovs;
                    
                    // predict the number of light samples that would have been taken if the ray hit geom
                    int totalLightSamples = fs.mIntegrator->getLightSampleCount() * fs.mScene->getLightCount();

                    // Doesn't matter what the lpe is -- if there are subpixels that hit a surface that isn't included
                    // in the lpe, this would be black anyway. If there are subpixels that DO hit a surface that is
                    // included in the lpe, this addition prevents aliasing. 
                    aovAccumVisibilityAttemptsBundled(pbrTls, aovSchema, lightAovs, totalLightSamples, 
                                                      rayStates[i]->mSubpixel.mPixel, rayStates[i]->mDeepDataHandle);
                }
            }
        } else {
            geom::internal::BVHUserData* userData =
                static_cast<geom::internal::BVHUserData*>(ray.ext.userData);
            const geom::internal::Primitive* prim = userData->mPrimitive;
            const scene_rdl2::rdl2::Material *rdl2Material = MNRY_VERIFY(prim)->getIntersectionMaterial(layer, ray);
            const shading::Material *material = &rdl2Material->get<const shading::Material>();

            if (material) {
                // perform material substitution if needed
                scene_rdl2::rdl2::RaySwitchContext switchCtx;
                switchCtx.mRayType = lobeTypeToRayType(pv.lobeType);
                rdl2Material = rdl2Material->raySwitch(switchCtx);
                material = &rdl2Material->get<const shading::Material>();

                sortedEntry.mSortKey = MNRY_VERIFY(material->getMaterialId());
                sortedEntry.mRsIdx = rayStates[i] - baseRayState;
                sortedEntry.mMaterial = material;
                maxSortKey = std::max(maxSortKey, sortedEntry.mSortKey);
                ++numSortedEntries;
            } else {
                // No material is assigned to this hit point, just skip entry
                // and free up associated RayState resource.
                rayStatesToFree[numRayStatesToFree++] = rayStates[i];

                // We may still have volume radiance to consider
                const RayState &rs = *rayStates[i];
                if (rs.mVolHit) {
                    // We passed through a volume and then hit a geometry.
                    // But there is no material assigned to the geometry, so
                    // there will be no further processing on this ray.  It will
                    // not be passed to the shade queue.
                    // We will add the radiance from the volume to the radiance
                    // queue and set the alpha based on the volume alpha.
                    const float alpha = ray.getDepth() == 0 ?
                        rs.mPathVertex.pathPixelWeight * (1.0f - reduceTransparency(rs.mVolTalpha)) : 0.f;
                    BundledRadiance rad;
                    rad.mRadiance = RenderColor(rs.mVolRad.r, rs.mVolRad.g, rs.mVolRad.b, alpha);
                    rad.mPathPixelWeight = rs.mPathVertex.pathPixelWeight;
                    rad.mPixel = rs.mSubpixel.mPixel;
                    rad.mSubPixelIndex = rs.mSubpixel.mSubpixelIndex;
                    rad.mDeepDataHandle = pbrTls->acquireDeepData(rs.mDeepDataHandle);
                    rad.mCryptomatteDataHandle = pbrTls->acquireCryptomatteData(rs.mCryptomatteDataHandle);
                    rad.mCryptoRefP = rs.mCryptoRefP;
                    rad.mCryptoRefN = rs.mCryptoRefN;
                    rad.mCryptoUV = rs.mCryptoUV;
                    rad.mTilePass = rs.mTilePass;
                    pbrTls->addRadianceQueueEntries(1, &rad);
                }
            }
        }
    }

    // Do the actual sorting.
    sortedEntries = scene_rdl2::util::smartSort32<SortedEntry, 0, RAY_HANDLER_STD_SORT_CUTOFF>(numSortedEntries,
                                                                                               sortedEntries,
                                                                                               maxSortKey, arena);

    //
    // The SortedEntry array in now sorted by material, with all the entries
    // which didn't hit anything or have a null material assigned at the start.
    //

    SortedEntry *endEntry = sortedEntries + numSortedEntries;
    unsigned numMisses = 0;

    // Aovs.
    float *aovs = nullptr;
    unsigned aovNumChannels = 0;
    if (!fs.mAovSchema->empty()) {
        // scratch space storage for per-pixel aov packing
        aovNumChannels = fs.mAovSchema->numChannels();
        aovs = arena->allocArray<float>(aovNumChannels);
        fs.mAovSchema->initFloatArray(aovs);
    }

    // Check if rays which didn't intersect anything hit any lights in the scene.
    if (sortedEntries->mSortKey == 0) {

        SortedEntry *spanEnd = sortedEntries + 1;
        while (spanEnd != endEntry && spanEnd->mSortKey == 0) {
            ++spanEnd;
        }

        numMisses = unsigned(spanEnd - sortedEntries);

        if (numMisses) {

            EXCL_ACCUMULATOR_PROFILE(pbrTls, EXCL_ACCUM_INTEGRATION);

            BundledRadiance *radiances = arena->allocArray<BundledRadiance>(numMisses, CACHE_LINE_SIZE);

            for (unsigned i = 0; i < numMisses; ++i) {

                RayState *rs = &baseRayState[sortedEntries[i].mRsIdx];
                // if the ray is not a primary ray and hit a light,
                // its radiance contribution will be tested in occlusion or
                // presence shadow ray queue
                scene_rdl2::math::Color radiance = scene_rdl2::math::sBlack;
                float alpha = 0.0f;
                BundledRadiance *rad = &radiances[i];

                // This code path only gets executed for rays which didn't intersect with any geometry
                // in the scene. We can discard such rays at this point but first need to check if
                // primary rays intersected any lights, so we can include their contribution if visible.

                const Light *hitLight = nullptr;
                if (rs->mRay.getDepth() == 0) {

                    LightIntersection hitLightIsect;
                    int numHits = 0;
                    const Light *hitLight;

                    SequenceIDIntegrator sid(rs->mSubpixel.mPixel,
                                             rs->mSubpixel.mSubpixelIndex,
                                             fs.mInitialSeed);
                    IntegratorSample1D lightChoiceSamples(sid);
                    hitLight = fs.mScene->intersectVisibleLight(rs->mRay,
                        sInfiniteLightDistance, lightChoiceSamples, hitLightIsect, numHits);

                    if (hitLight) {
                        // Evaluate the radiance on the selected light in camera.
                        // Note: we multiply the radiance contribution by the number of lights hit. This is
                        // because we want to compute the sum of all contributing lights, but we're
                        // stochastically sampling just one.

                        LightFilterRandomValues lightFilterR = {
                            scene_rdl2::math::Vec2f(0.f, 0.f), 
                            scene_rdl2::math::Vec3f(0.f, 0.f, 0.f)}; // light filters don't apply to camera rays
                        radiance = rs->mPathVertex.pathThroughput *
                            hitLight->eval(tls, rs->mRay.getDirection(), rs->mRay.getOrigin(),
                                           lightFilterR, rs->mRay.getTime(), hitLightIsect, true, nullptr,
                                           rs->mRay.getDirFootprint()) * numHits;
                        // attenuate based on volume transmittance
                        if (rs->mVolHit) radiance *= (rs->mVolTr * rs->mVolTh);

                        // alpha depends on light opacity and volumes
                        if (hitLight->getIsOpaqueInAlpha()) {
                            // We hit a visible light that is opaque in alpha.
                            // Volumes are irrelevant, the alpha contribution is
                            // the full pixel weight.
                            alpha = rs->mPathVertex.pathPixelWeight;
                        } else if (rs->mVolHit) {
                            // We hit a visible light, but the light is not
                            // opaque in alpha (e.g. a distant or env light).
                            // There is a volume along this ray.  The volume
                            // alpha transmission determines the alpha contribution.
                            alpha = rs->mPathVertex.pathPixelWeight * (1.f - reduceTransparency(rs->mVolTalpha));
                        } else {
                            // We hit a visible light, but the light is not
                            // opaque in alpha (e.g. a distant or env light).
                            // There is no volume along the ray.
                            // The alpha contribution is 0.
                            alpha = 0.f;
                        }
                    } else if (rs->mVolHit) {
                        // We didn't hit a visible light.  We didn't hit geometry.
                        // But we did pass through a volume.
                        // The volume alpha transmission determines the alpha contribution.
                        alpha = rs->mPathVertex.pathPixelWeight * (1.f - reduceTransparency(rs->mVolTalpha));
                    }
                }

                // add in any volume radiance
                radiance += rs->mVolRad;

                rad->mRadiance = RenderColor(radiance.r, radiance.g, radiance.b, alpha);
                rad->mPathPixelWeight = rs->mPathVertex.pathPixelWeight;
                rad->mPixel = rs->mSubpixel.mPixel;
                rad->mSubPixelIndex = rs->mSubpixel.mSubpixelIndex;
                rad->mDeepDataHandle = pbrTls->acquireDeepData(rs->mDeepDataHandle);
                rad->mCryptomatteDataHandle = pbrTls->acquireCryptomatteData(rs->mCryptomatteDataHandle);
                rad->mCryptoRefP = rs->mCryptoRefP;
                rad->mCryptoRefN = rs->mCryptoRefN;
                rad->mCryptoUV = rs->mCryptoUV;
                rad->mTilePass = rs->mTilePass;

                // LPE
                if (!fs.mAovSchema->empty()) {
                    EXCL_ACCUMULATOR_PROFILE(pbrTls, EXCL_ACCUM_AOVS);

                    // accumulate background aovs
                    aovAccumBackgroundExtraAovsBundled(pbrTls, fs, rs);

                    // Did we hit a volume and do we have volume depth/position AOVs?
                    if (rs->mRay.getDepth() == 0 && rs->mVolHit && rs->mVolumeSurfaceT < scene_rdl2::math::sMaxValue) {
                        aovSetStateVarsVolumeOnly(pbrTls,
                                                  *fs.mAovSchema,
                                                  rs->mVolumeSurfaceT,
                                                  rs->mRay,
                                                  *fs.mScene,
                                                  rs->mPathVertex.pathPixelWeight,
                                                  aovs);
                        aovAddToBundledQueueVolumeOnly(pbrTls,
                                                       *fs.mAovSchema,
                                                       rs->mRay,
                                                       AOV_TYPE_STATE_VAR,
                                                       aovs,
                                                       rs->mSubpixel.mPixel,
                                                       rs->mDeepDataHandle);
                    }

                    const LightAovs &lightAovs = *fs.mLightAovs;
                    // This is complicated.
                    // Case 1:
                    // rayDepth() == 0.  In this case, the ray left
                    // the camera, and hit a light.  We use the lpeStateId in
                    // in the ray state.
                    //
                    // Case 2:
                    // We expect that PathIntegratorBundled set lpeStateId to
                    // the scattering event, and lpeStateIdLight to the light
                    // event.  In this case we hit no geometry, so we hit the light.
                    // For this reason, we use lpeStateIdLight rather than lpeStateId
                    //
                    int lpeStateId = -1;
                    if (rs->mRay.getDepth() == 0) {
                        if (hitLight) {
                            // case 1
                            int lpeStateId = rs->mPathVertex.lpeStateId;
                            if (lpeStateId >= 0) {
                                // transition to light event
                                lpeStateId = lightAovs.lightEventTransition(pbrTls, lpeStateId, hitLight);
                            }
                        }
                    } else {
                        // case 2
                        // transition already computed in PathIntegratorBundled
                        lpeStateId = rs->mPathVertex.lpeStateIdLight;
                    }

                    if (lpeStateId >= 0) {
                        // accumulate results. As this has to do with directly hitting a light, we don't have
                        // to worry about pre-occlusion LPEs here.
                        aovAccumLightAovsBundled(pbrTls, *fs.mAovSchema,
                                                 lightAovs, radiance, nullptr, AovSchema::sLpePrefixNone, lpeStateId,
                                                 rad->mPixel, rad->mDeepDataHandle);
                    }
                }

                // It's critical that we don't leak ray states.
                rayStatesToFree[numRayStatesToFree++] = rs;
            }

            pbrTls->addRadianceQueueEntries(numMisses, radiances);

            CHECK_CANCELLATION(pbrTls, return);
        }
    }

    // Bulk free raystates.
    MNRY_ASSERT(numRayStatesToFree <= numEntries);
    pbrTls->freeRayStates(numRayStatesToFree, rayStatesToFree);

    //
    // Route remaining sortedEntries to their associated materials in batches.
    // Shade queues are thread safe, multiple threads can add to them simultaneously.
    //

    uint8_t *memBookmark = arena->getPtr();

    SortedEntry *currEntry = sortedEntries + numMisses;
    while (currEntry != endEntry) {

        const uint32_t currBundledMatId = MNRY_VERIFY(currEntry->mSortKey);

        SortedEntry *spanEnd = currEntry + 1;
        while (spanEnd != endEntry && spanEnd->mSortKey == currBundledMatId) {
            ++spanEnd;
        }

        // Create entries for shade queue.
        unsigned numShadeEntries = MNRY_VERIFY(spanEnd - currEntry);
        shading::SortedRayState *shadeEntries = arena->allocArray<shading::SortedRayState>(numShadeEntries,
                                                                                           CACHE_LINE_SIZE);

        for (unsigned i = 0; i < numShadeEntries; ++i) {
            uint32_t rsIdx = currEntry[i].mRsIdx;
            RayState *rs = &baseRayState[rsIdx];
            const mcrt_common::Ray &ray = rs->mRay;
            shadeEntries[i].mRsIdx = rsIdx;

            // Sort first by geometry and then by primitive within that geometry.
            // This is to improve locality for postIntersection calls.
            shadeEntries[i].mSortKey = ((ray.geomID & 0xfff) << 20) | (ray.primID & 0xfffff);
        }

        // Submit to destination queue.
        shading::ShadeQueue *shadeQueue = MNRY_VERIFY(currEntry->mMaterial->getShadeQueue());
        shadeQueue->addEntries(tls, numShadeEntries, shadeEntries, arena);

        CHECK_CANCELLATION(pbrTls, return);

        // Free up entry memory from arena.
        arena->setPtr(memBookmark);

        currEntry = spanEnd;
    }
}

void
occlusionQueryBundleHandler(mcrt_common::ThreadLocalState *tls, unsigned numEntries,
                            BundledOcclRay **entries, void *userData)
{
    pbr::TLState *pbrTls = tls->mPbrTls.get();

    EXCL_ACCUMULATOR_PROFILE(pbrTls, EXCL_ACCUM_OCCL_QUERY_HANDLER);

    scene_rdl2::alloc::Arena *arena = &tls->mArena;
    SCOPED_MEM(arena);

    RayHandlerFlags handlerFlags = RayHandlerFlags((uint64_t)userData);

    BundledRadiance *results = arena->allocArray<BundledRadiance>(numEntries, CACHE_LINE_SIZE);

    unsigned numRadiancesFilled = computeOcclusionQueriesBundled(pbrTls,
                                        numEntries, entries, results, handlerFlags);

    MNRY_ASSERT(numRadiancesFilled <= numEntries);

    CHECK_CANCELLATION(pbrTls, return);

    pbrTls->addRadianceQueueEntries(numRadiancesFilled, results);
}

void
presenceShadowsQueryBundleHandler(mcrt_common::ThreadLocalState *tls, unsigned numEntries,
                                  BundledOcclRay **entries, void *userData)
{
    // Presence handling code for direct lighting
    pbr::TLState *pbrTls = tls->mPbrTls.get();

    EXCL_ACCUMULATOR_PROFILE(pbrTls, EXCL_ACCUM_PRESENCE_QUERY_HANDLER);

    scene_rdl2::alloc::Arena *arena = &tls->mArena;
    SCOPED_MEM(arena);

    RayHandlerFlags handlerFlags = RayHandlerFlags((uint64_t)userData);

    BundledRadiance *results = arena->allocArray<BundledRadiance>(numEntries, CACHE_LINE_SIZE);

    unsigned numRadiancesFilled = computePresenceShadowsQueriesBundled(pbrTls,
                                        numEntries, entries, results, handlerFlags);

    MNRY_ASSERT(numRadiancesFilled <= numEntries);

    CHECK_CANCELLATION(pbrTls, return);

    pbrTls->addRadianceQueueEntries(numRadiancesFilled, results);
}

//-----------------------------------------------------------------------------

#pragma warning pop

} // namespace pbr
} // namespace moonray

