"""Generate a narrow benchmark-only SHA reuse shim; never edits production sources."""
from pathlib import Path

root=Path(__file__).resolve().parents[2]
source=(root/'src/cursor_video.cpp').read_text(encoding='utf-8')
old='bool Identity(HANDLE file, uint64_t& bytes, std::string& digest) {'
new='''struct PerfPrepared { HANDLE held=INVALID_HANDLE_VALUE; uint64_t bytes=0; std::string digest; unsigned computed=0,reused=0; };
PerfPrepared* perfContext=nullptr;
bool SameHeldFile(HANDLE file) {
    BY_HANDLE_FILE_INFORMATION a{},b{};
    return perfContext && GetFileInformationByHandle(file,&a) && GetFileInformationByHandle(perfContext->held,&b) &&
      a.dwVolumeSerialNumber==b.dwVolumeSerialNumber && a.nFileIndexHigh==b.nFileIndexHigh && a.nFileIndexLow==b.nFileIndexLow;
}
bool OriginalIdentity(HANDLE file, uint64_t& bytes, std::string& digest) {'''
assert source.count(old)==1
source=source.replace(old,new)
old='if (hash) CryptDestroyHash(hash); CryptReleaseContext(provider, 0); return ok;\n}'
new=old+'''
bool Identity(HANDLE file,uint64_t& bytes,std::string& digest) {
    const bool eligible=SameHeldFile(file);
    if(eligible&&!perfContext->digest.empty()){bytes=perfContext->bytes;digest=perfContext->digest;++perfContext->reused;return true;}
    const bool ok=OriginalIdentity(file,bytes,digest);
    if(ok&&eligible){perfContext->bytes=bytes;perfContext->digest=digest;++perfContext->computed;}
    return ok;
}'''
assert source.count(old)==1
source=source.replace(old,new)
out=root/'build/perf/video/video_prepared_engine.inc';out.parent.mkdir(parents=True,exist_ok=True);out.write_text(source,encoding='utf-8')
print(out)
