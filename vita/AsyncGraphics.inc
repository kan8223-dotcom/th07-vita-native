namespace {
class AsyncGraphics final:public ZunGraphics {
#if defined(TH07_REUSE_COMMAND_STORAGE) && TH07_REUSE_COMMAND_STORAGE
    th07_pipeline::ReusableFrameWorker worker;
#else
    touhou::vitacore::FrameWorker worker;
#endif
    touhou::vitacore::FrameWorker::Tasks commands;
    std::unique_ptr<VitaGraphics> backend;
    std::atomic<bool> failed{false};bool closed=false;
    std::atomic<unsigned> completedFrames{0};
    ZunViewport view{0,0,640,480,0,1};
    uint64_t frames=0,waitUs=0,vertexBytes=0;
#if defined(TH07_SNAPSHOT_ARENA) && TH07_SNAPSHOT_ARENA
    std::array<th07_snapshot::Arena,2> snapshots;
    unsigned snapshotSlot=0;
    size_t snapshot(const void* data,size_t bytes){
        const auto offset=snapshots[snapshotSlot].append(data,bytes);
        if(offset==th07_snapshot::Arena::invalid){failed=true;th07_vita_stop("frame snapshot arena budget exceeded");}
        return offset;
    }
#endif
    void flushSprites(){if(g_AnmManager&&g_AnmManager->spritesToDraw)g_AnmManager->Flush();}
    template<class F> void command(F action){commands.emplace_back([this,action=std::move(action)]{
        if(!failed.load(std::memory_order_relaxed)){action(*backend);if(backend->hasError())failed.store(true,std::memory_order_release);}
    });}
    template<class Method,class... Args> void post(Method method,Args... args){command([method,args...](VitaGraphics& b){(b.*method)(args...);});}
    void publish(){
        if(commands.empty())return;const auto t=nowUs();
#if defined(TH07_REUSE_COMMAND_STORAGE) && TH07_REUSE_COMMAND_STORAGE
        worker.submit(commands);
#else
        worker.submit(std::move(commands));
#endif
        commands.clear();waitUs+=nowUs()-t;
#if defined(TH07_SNAPSHOT_ARENA) && TH07_SNAPSHOT_ARENA
        // submit waits for the previous packet before accepting this one.
        // The other arena is therefore no longer read by the render thread.
        snapshotSlot^=1;snapshots[snapshotSlot].reset();
#endif
    }
    void barrier(){publish();worker.drain();}
public:
    bool initialize(){
        const int state=g_Supervisor.curState,stage=g_GameManager.currentStage,frame=state==2?g_GameManager.framesThisStage:-1;
        const bool fit=g_Supervisor.cfg.windowed;
        const unsigned controls=touhou::vitacore::mathControls();
        return worker.start([this,state,stage,frame,fit,controls]{
            if(!touhou::vitacore::bind("render-prepare-gxm",1))return false;
            touhou::vitacore::inheritMathControls(controls);
            backend.reset(new VitaGraphics);backend->frameInfo(state,stage,frame,fit);
            const bool ok=backend->initialize();failed=!ok;touhou::vitacore::snapshot("renderer-ready");return ok;
        },[this]{backend.reset();});
    }
    ~AsyncGraphics()override{Exit();}
    void Exit()override{if(closed)return;barrier();worker.stop();closed=true;}
    RendererType GetType()override{return RENDERER_GXM;}
    void SetFogRange(float a,float b)override{post(&VitaGraphics::SetFogRange,a,b);}
    void SetFogColor(ZunColor a)override{post(&VitaGraphics::SetFogColor,a);}
    void SetColorOp(TextureOpComponent a,ColorOp b)override{post(&VitaGraphics::SetColorOp,a,b);}
    void SetTextureFactor(ZunColor a)override{post(&VitaGraphics::SetTextureFactor,a);}
    void SetTextureArg(TextureArg a)override{post(&VitaGraphics::SetTextureArg,a);}
    void SetTransformMatrix(TransformMatrix a,const ZunMatrix& b)override{post(&VitaGraphics::SetTransformMatrix,a,b);}
    void SetTextureFilter()override{post(&VitaGraphics::SetTextureFilter);}
    void GetViewport(ZunViewport& v)override{v=view;}
    void SetViewport(const ZunViewport& v)override{view=v;post(&VitaGraphics::SetViewport,v);}
    void Enable(Capabilities a)override{post(&VitaGraphics::Enable,a);}
    void Disable(Capabilities a)override{post(&VitaGraphics::Disable,a);}
    void SetBlendMode(BlendMode a,BlendMode b)override{flushSprites();post(&VitaGraphics::SetBlendMode,a,b);}
    void SetDepthMask(bool a)override{flushSprites();post(&VitaGraphics::SetDepthMask,a);}
    void SetDepthFunc(DepthFunc a)override{flushSprites();post(&VitaGraphics::SetDepthFunc,a);}
    void SetClearDepth(float a)override{post(&VitaGraphics::SetClearDepth,a);}
    void SetClearColor(ZunColor a)override{post(&VitaGraphics::SetClearColor,a);}
    void SetAlphaTestRef(u8 a)override{post(&VitaGraphics::SetAlphaTestRef,a);}
    void Clear(u32 a)override{post(&VitaGraphics::Clear,a);}
    GfxTextureHandle CreateTexture()override{GfxTextureHandle result;command([&result](VitaGraphics& b){result=b.CreateTexture();});barrier();return result;}
    void BindTexture(GfxTextureHandle a)override{post(&VitaGraphics::BindTexture,a);}
    void DeleteTexture(GfxTextureHandle a)override{post(&VitaGraphics::DeleteTexture,a);}
    void SetTextureImage(u32 w,u32 h,PixelFormat f,PixelDataType t,const void* p)override{
        const size_t bytes=t==PIXEL_UNSIGNED_BYTE?(f==PIXEL_RGB?3:4):2;
#if defined(TH07_SNAPSHOT_ARENA) && TH07_SNAPSHOT_ARENA
        const auto* arena=&snapshots[snapshotSlot];
        const size_t imageBytes=size_t(w)*h*bytes;const bool present=p&&imageBytes;
        const size_t offset=present?snapshot(p,imageBytes):0;
        if(present&&offset==th07_snapshot::Arena::invalid)return;
        command([w,h,f,t,arena,offset,present](VitaGraphics& b){b.SetTextureImage(w,h,f,t,present?arena->at(offset):nullptr);});
#else
        std::vector<u8> copy;if(p)copy.assign(static_cast<const u8*>(p),static_cast<const u8*>(p)+size_t(w)*h*bytes);
        command([w,h,f,t,copy=std::move(copy)](VitaGraphics& b){b.SetTextureImage(w,h,f,t,copy.empty()?nullptr:copy.data());});
#endif
    }
    void SetTextureSubImage(int x,int y,int w,int h,const void* p)override{
        if(!p||w<=0||h<=0)return;
#if defined(TH07_SNAPSHOT_ARENA) && TH07_SNAPSHOT_ARENA
        const auto* arena=&snapshots[snapshotSlot];const auto offset=snapshot(p,size_t(w)*h*4);
        if(offset==th07_snapshot::Arena::invalid)return;
        command([x,y,w,h,arena,offset](VitaGraphics& b){b.SetTextureSubImage(x,y,w,h,arena->at(offset));});
#else
        std::vector<u8> copy(static_cast<const u8*>(p),static_cast<const u8*>(p)+size_t(w)*h*4);
        command([x,y,w,h,copy=std::move(copy)](VitaGraphics& b){b.SetTextureSubImage(x,y,w,h,copy.data());});
#endif
    }
    void ReadPixels(int x,int y,int w,int h,void* p)override{post(&VitaGraphics::ReadPixels,x,y,w,h,p);barrier();}
    void DrawPrimitive(PrimitiveType t,int first,int n)override{post(&VitaGraphics::DrawPrimitive,t,first,n);}
    void DrawPrimitiveUP(PrimitiveType t,int n,const void* p,int stride)override{
        if(!p||n<=0||stride<=0)return;
        const size_t count=t==PRIM_TRIANGLES?size_t(n)*3:t==PRIM_QUADS?size_t(n)*4:size_t(n)+2;
        if(count>size_t(16u*1024u*1024u)/unsigned(stride)){failed=true;th07_vita_stop("invalid frame snapshot size");return;}
#if defined(TH07_SNAPSHOT_ARENA) && TH07_SNAPSHOT_ARENA
        const auto* arena=&snapshots[snapshotSlot];const auto offset=snapshot(p,count*stride);
        if(offset==th07_snapshot::Arena::invalid)return;vertexBytes+=count*stride;
        command([t,n,stride,arena,offset](VitaGraphics& b){b.DrawPrimitiveUP(t,n,arena->at(offset),stride);});
#else
        std::vector<u8> copy(static_cast<const u8*>(p),static_cast<const u8*>(p)+count*stride);vertexBytes+=copy.size();
        command([t,n,stride,copy=std::move(copy)](VitaGraphics& b){b.DrawPrimitiveUP(t,n,copy.data(),stride);});
#endif
    }
    void SwapBuffers()override{
        const int nextState=g_Supervisor.curState,nextStage=g_GameManager.currentStage,nextFrame=nextState==2?g_GameManager.framesThisStage:-1;
        const bool fit=g_Supervisor.cfg.windowed;
        command([this,nextState,nextStage,nextFrame,fit](VitaGraphics& b){b.outputFit(fit);b.SwapBuffers();if(!b.hasError())completedFrames.fetch_add(1,std::memory_order_release);b.frameInfo(nextState,nextStage,nextFrame,fit);});
        publish();++frames;th07_vita_presented();
        if(frames%600==0)waitUs=vertexBytes=0;
    }
};
}
