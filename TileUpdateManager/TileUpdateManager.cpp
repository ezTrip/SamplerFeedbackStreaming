//*********************************************************
//
// Copyright 2020 Intel Corporation 
//
// Permission is hereby granted, free of charge, to any 
// person obtaining a copy of this software and associated 
// documentation files(the "Software"), to deal in the Software 
// without restriction, including without limitation the rights 
// to use, copy, modify, merge, publish, distribute, sublicense, 
// and/or sell copies of the Software, and to permit persons to 
// whom the Software is furnished to do so, subject to the 
// following conditions :
// The above copyright notice and this permission notice shall 
// be included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT 
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, 
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, 
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
// DEALINGS IN THE SOFTWARE.
//
//*********************************************************

//=============================================================================
// Implementation of the ::TileUpdateManager public (external) interface
//=============================================================================

#include "pch.h"

#include "TileUpdateManagerBase.h"
#include "TileUpdateManagerSR.h"
#include "StreamingResourceBase.h"
#include "DataUploader.h"
#include "StreamingHeap.h"

//--------------------------------------------
// instantiate streaming library
//--------------------------------------------
TileUpdateManager* TileUpdateManager::Create(const TileUpdateManagerDesc& in_desc)
{
    Streaming::ComPtr<ID3D12Device8> device;
    in_desc.m_pDirectCommandQueue->GetDevice(IID_PPV_ARGS(&device));
    return new Streaming::TileUpdateManagerBase(in_desc, device.Get());
}

void Streaming::TileUpdateManagerBase::Destroy()
{
    delete this;
}

//--------------------------------------------
// Create a heap used by 1 or more StreamingResources
// parameter is number of 64KB tiles to manage
//--------------------------------------------
StreamingHeap* Streaming::TileUpdateManagerBase::CreateStreamingHeap(UINT in_maxNumTilesHeap)
{
    auto pStreamingHeap = new Streaming::Heap(m_dataUploader.GetMappingQueue(), in_maxNumTilesHeap);
    return (StreamingHeap*)pStreamingHeap;
}

//--------------------------------------------
// Create StreamingResources using a common TileUpdateManager
//--------------------------------------------
StreamingResource* Streaming::TileUpdateManagerBase::CreateStreamingResource(const std::wstring& in_filename, StreamingHeap* in_pHeap)
{
    // if threads are running, stop them. they have state that depends on knowing the # of StreamingResources
    Finish();

    Streaming::FileHandle* pFileHandle = m_dataUploader.OpenFile(in_filename);
    auto pRsrc = new Streaming::StreamingResourceBase(in_filename, pFileHandle, (Streaming::TileUpdateManagerSR*)this, (Streaming::Heap*)in_pHeap);
    m_streamingResources.push_back(pRsrc);
    m_numStreamingResourcesChanged = true;

    m_havePackedMipsToLoad = true;

    return (StreamingResource*)pRsrc;
}

//-----------------------------------------------------------------------------
// set which file streaming system to use
// will reset even if previous setting was the same. so?
//-----------------------------------------------------------------------------
void Streaming::TileUpdateManagerBase::UseDirectStorage(bool in_useDS)
{
    Finish();
    auto streamerType = Streaming::DataUploader::StreamerType::Reference;
    if (in_useDS)
    {
        streamerType = Streaming::DataUploader::StreamerType::DirectStorage;
    }

    auto pOldStreamer = m_dataUploader.SetStreamer(streamerType);

    for (auto& s : m_streamingResources)
    {
        s->SetFileHandle(&m_dataUploader);
    }

    delete pOldStreamer;
}

//-----------------------------------------------------------------------------
// note to self to create Clear() and Resolve() commands during EndFrame()
//-----------------------------------------------------------------------------
void Streaming::TileUpdateManagerBase::QueueFeedback(StreamingResource* in_pResource, D3D12_GPU_DESCRIPTOR_HANDLE in_gpuDescriptor)
{
    auto pResource = (Streaming::StreamingResourceBase*)in_pResource;

    m_feedbackReadbacks.push_back({ pResource, in_gpuDescriptor });

    // add feedback clears
    pResource->ClearFeedback(GetCommandList(CommandListName::Before), in_gpuDescriptor);

    // barrier coalescing around blocks of commands in EndFrame():

    // after drawing, transition the opaque feedback resources from UAV to resolve source
    // transition the feedback decode target to resolve_dest
    m_barrierUavToResolveSrc.push_back(CD3DX12_RESOURCE_BARRIER::Transition(pResource->GetOpaqueFeedback(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RESOLVE_SOURCE));

    // after resolving, transition the opaque resources back to UAV. Transition the resolve destination to copy source for read back on cpu
    m_barrierResolveSrcToUav.push_back(CD3DX12_RESOURCE_BARRIER::Transition(pResource->GetOpaqueFeedback(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

#if RESOLVE_TO_TEXTURE
    // resolve to texture incurs a subsequent copy to linear buffer
    m_barrierUavToResolveSrc.push_back(CD3DX12_RESOURCE_BARRIER::Transition(pResource->GetResolvedFeedback(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RESOLVE_DEST));
    m_barrierResolveSrcToUav.push_back(CD3DX12_RESOURCE_BARRIER::Transition(pResource->GetResolvedFeedback(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE));
#endif
}

//-----------------------------------------------------------------------------
// returns (approximate) cpu time for processing feedback in the previous frame
// since processing happens asynchronously, this time should be averaged
//-----------------------------------------------------------------------------
float Streaming::TileUpdateManagerBase::GetCpuProcessFeedbackTime()
{
    return m_processFeedbackFrameTime;
}

//-----------------------------------------------------------------------------
// performance and visualization
//-----------------------------------------------------------------------------
float Streaming::TileUpdateManagerBase::GetGpuStreamingTime() const { return m_dataUploader.GetGpuStreamingTime(); }
float Streaming::TileUpdateManagerBase::GetTotalTileCopyLatency() const { return m_dataUploader.GetApproximateTileCopyLatency(); }

// the total time the GPU spent resolving feedback during the previous frame
float Streaming::TileUpdateManagerBase::GetGpuTime() const { return m_gpuTimerResolve.GetTimes()[m_renderFrameIndex].first; }
UINT Streaming::TileUpdateManagerBase::GetTotalNumUploads() const { return m_dataUploader.GetTotalNumUploads(); }
UINT Streaming::TileUpdateManagerBase::GetTotalNumEvictions() const { return m_dataUploader.GetTotalNumEvictions(); }
UINT Streaming::TileUpdateManagerBase::GetTotalNumSubmits() const { return m_numTotalSubmits; }

void Streaming::TileUpdateManagerBase::SetVisualizationMode(UINT in_mode)
{
    ASSERT(!GetWithinFrame());
    Finish();
    for (auto o : m_streamingResources)
    {
        o->ClearAllocations();
    }

    m_dataUploader.SetVisualizationMode(in_mode);
}

void Streaming::TileUpdateManagerBase::CaptureTraceFile(bool in_captureTrace)
{
    m_dataUploader.CaptureTraceFile(in_captureTrace);
}

//-----------------------------------------------------------------------------
// Call this method once for each TileUpdateManager that shares heap/upload buffers
// expected to be called once per frame, before anything is drawn.
//-----------------------------------------------------------------------------
void Streaming::TileUpdateManagerBase::BeginFrame(ID3D12DescriptorHeap* in_pDescriptorHeap,
    D3D12_CPU_DESCRIPTOR_HANDLE in_minmipmapDescriptorHandle)
{
    ASSERT(!GetWithinFrame());
    m_withinFrame = true;

    StartThreads();

    m_processFeedbackFlag.Set();

    // if new StreamingResources have been created...
    if (m_numStreamingResourcesChanged)
    {
        m_numStreamingResourcesChanged = false;
        AllocateResidencyMap(in_minmipmapDescriptorHandle);
    }

    // the frame fence is used to optimize readback of feedback
    // only read back the feedback after the frame that writes to it has completed
    // note the signal is for the previous frame, the value is for "this" frame
    m_directCommandQueue->Signal(m_frameFence.Get(), m_frameFenceValue);
    m_frameFenceValue++;

    m_renderFrameIndex = (m_renderFrameIndex + 1) % m_numSwapBuffers;
    for (auto& cl : m_commandLists)
    {
        auto& allocator = cl.m_allocators[m_renderFrameIndex];
        allocator->Reset();
        ThrowIfFailed(cl.m_commandList->Reset(allocator.Get(), nullptr));
    }
    ID3D12DescriptorHeap* ppHeaps[] = { in_pDescriptorHeap };
    GetCommandList(CommandListName::Before)->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

    // capture cpu time spent processing feedback
    {
        INT64 processFeedbackTime = m_processFeedbackTime; // snapshot of total time spent
        m_processFeedbackFrameTime = m_cpuTimer.GetSecondsFromDelta(processFeedbackTime - m_previousFeedbackTime);
        m_previousFeedbackTime = processFeedbackTime; // remember current time for next call
    }

}

//-----------------------------------------------------------------------------
// Call this method once corresponding to BeginFrame()
// expected to be called once per frame, after everything was drawn.
//-----------------------------------------------------------------------------
TileUpdateManager::CommandLists Streaming::TileUpdateManagerBase::EndFrame()
{
    ASSERT(GetWithinFrame());
    // NOTE: we are "within frame" until the end of EndFrame()

    // transition packed mips if necessary
    // FIXME? if any 1 needs a transition, go ahead and check all of them. not worth optimizing.
    // NOTE: the debug layer will complain about CopyTextureRegion() if the resource state is not state_copy_dest (or common)
    //       despite the fact the copy queue doesn't really care about resource state
    //       CopyTiles() won't complain because this library always targets an atlas that is always state_copy_dest
    if (m_packedMipTransition)
    {
        m_packedMipTransition = false;
        for (auto o : m_streamingResources)
        {
            if (o->GetPackedMipsNeedTransition())
            {
                D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
                    o->GetTiledResource(),
                    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                m_packedMipTransitionBarriers.push_back(b);
            }
        }
    }

    //------------------------------------------------------------------
    // before draw calls, do the following:
    //     - clear feedback buffers
    //     - resource barriers for aliasing and packed mip transitions
    //------------------------------------------------------------------
    {
        auto pCommandList = GetCommandList(CommandListName::Before);

        /*
        * Aliasing barriers are unnecessary, as draw commands only access modified resources after a fence has signaled on the copy queue
        * Note it is also theoretically possible for tiles to be re-assigned while a draw command is executing
        * However, performance analysis tools like to know about changes to resources
        */
        if ((m_addAliasingBarriers) && (m_streamingResources.size()))
        {
            m_aliasingBarriers.reserve(m_streamingResources.size());
            m_aliasingBarriers.resize(0);
            for (auto pResource : m_streamingResources)
            {
                m_aliasingBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Aliasing(nullptr, pResource->GetTiledResource()));
            }

            pCommandList->ResourceBarrier((UINT)m_aliasingBarriers.size(), m_aliasingBarriers.data());
        }

        // get any packed mip transition barriers accumulated by DataUploader
        if (m_packedMipTransitionBarriers.size())
        {
            pCommandList->ResourceBarrier((UINT)m_packedMipTransitionBarriers.size(), m_packedMipTransitionBarriers.data());
            m_packedMipTransitionBarriers.clear();
        }

#if COPY_RESIDENCY_MAPS
        // FIXME: would rather update multiple times per frame, and only affected regions
        D3D12_RESOURCE_BARRIER residencyBarrier = CD3DX12_RESOURCE_BARRIER::Transition(m_residencyMapLocal.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        pCommandList->ResourceBarrier(1, &residencyBarrier);

        pCommandList->CopyResource(m_residencyMapLocal.Get(), m_residencyMap.m_resource.Get());

        std::swap(residencyBarrier.Transition.StateBefore, residencyBarrier.Transition.StateAfter);
        pCommandList->ResourceBarrier(1, &residencyBarrier);
#endif
        pCommandList->Close();
    }

    //------------------------------------------------------------------
    // after draw calls,
    // resolve feedback and copy to readback buffers
    //------------------------------------------------------------------
    {
        auto pCommandList = GetCommandList(CommandListName::After);

        if (m_feedbackReadbacks.size())
        {
            m_gpuTimerResolve.BeginTimer(pCommandList, m_renderFrameIndex);

            // transition all feedback resources UAV->RESOLVE_SOURCE
            // also transition the (non-opaque) resolved resources COPY_SOURCE->RESOLVE_DEST
            pCommandList->ResourceBarrier((UINT)m_barrierUavToResolveSrc.size(), m_barrierUavToResolveSrc.data());
            m_barrierUavToResolveSrc.clear();

            // do the feedback resolves
            for (auto& t : m_feedbackReadbacks)
            {
                t.m_pStreamingResource->ResolveFeedback(pCommandList);
            }

            // transition all feedback resources RESOLVE_SOURCE->UAV
            // also transition the (non-opaque) resolved resources RESOLVE_DEST->COPY_SOURCE
            pCommandList->ResourceBarrier((UINT)m_barrierResolveSrcToUav.size(), m_barrierResolveSrcToUav.data());
            m_barrierResolveSrcToUav.clear();

            m_gpuTimerResolve.EndTimer(pCommandList, m_renderFrameIndex);
#if RESOLVE_TO_TEXTURE
            // copy readable feedback buffers to cpu
            for (auto& t : m_feedbackReadbacks)
            {
                t.m_pStreamingResource->ReadbackFeedback(pCommandList);
            }
#endif
            m_feedbackReadbacks.clear();

            m_gpuTimerResolve.ResolveTimer(pCommandList, m_renderFrameIndex);
        }

        pCommandList->Close();
    }

    TileUpdateManager::CommandLists outputCommandLists;
    outputCommandLists.m_beforeDrawCommands = m_commandLists[(UINT)CommandListName::Before].m_commandList.Get();
    outputCommandLists.m_afterDrawCommands = m_commandLists[(UINT)CommandListName::After].m_commandList.Get();

    m_withinFrame = false;

    return outputCommandLists;
}
