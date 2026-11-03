"""Extract production bodies for isolated tests; never substitute a hand-written implementation."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "artifacts/validation/finished-fg"
OUT.mkdir(parents=True, exist_ok=True)


def read(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


def function(source, signature):
    if source.count(signature) != 1:
        raise RuntimeError(f"Expected one production definition: {signature}")
    start = source.index(signature)
    # Mask comments, escaped string/character literals and C++ raw strings without changing
    # offsets; braces in those tokens must not terminate the extracted production function.
    token = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\([\s\S]*?\)(?P=delimiter)"|"(?:\\[\s\S]|[^"\\])*"|\'(?:\\[\s\S]|[^\'\\])*\'')
    code = token.sub(lambda match: " " * len(match.group()), source)
    brace = code.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        if end >= len(code):
            raise RuntimeError(f"Unbalanced production definition: {signature}")
        depth += (code[end] == "{") - (code[end] == "}")
        end += 1
    return source[start:end] + "\n"


def write(name, body):
    (OUT / name).write_text(body, encoding="utf-8", newline="\n")


# Extraction regression: braces in comments, normal/escaped/raw strings and characters.
example = r'''void sample() { // }
    /* { */ const char* a = "}\\\"{";
    const char* b = R"tag({ ") } )tag"; char c = '}';
    if (true) { /* } */ }
}'''
assert function(example + "\nvoid after() {}", "void sample(").strip() == example

late = read("OptiScaler/shaders/dlssnr/DlssNr_Late.inl")
write("FinishedReset.inl", function(late, "void RequestReset(") + function(late, "void FinishReset("))
shader = read("OptiScaler/shaders/Shader_Dx12.cpp")
compose = read("OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp")
hooks = read("OptiScaler/hooks/Streamline_Hooks.cpp")
native = read("OptiScaler/native/NativeDriverDx12.cpp")
write("FinishedPluginLookup.inl", function(hooks, "static void* FinishedPicturePluginFunction("))
write("FinishedNativeDriver.inl", function(native, "void RunFrame("))
write("FinishedNativePause.inl", function(read("OptiScaler/native/NativeDriver.cpp"), "void NativeDriver::PauseForNativeFg(")
      .replace("NativeDriver::", "Driver::"))
# Exercise the actual native-input early guards; the device/NR body after those guards is
# outside this no-inference harness. Assert the delimiter so a moved guard cannot be omitted.
entry = function(late, "static bool ApplyFinishedPictureInput(")
cut = entry.index("    Late::ComPtr<ID3D12Device> currentDevice;")
if "StreamlinePicture::NativeFgRunning()" not in entry[:cut]:
    raise RuntimeError("Native-input FG guard moved out of the tested entry")
write("FinishedNativeEntry.inl", entry[:cut] + "    return true; // stop before device/NR work\n}\n")
write("FinishedSubmit.inl", function(late, "void Submitted("))
write("FinishedSelect.inl", function(late, "static FinishedPick SelectFinishedInput("))
write("FinishedEnd.inl", function(late, "void FinishedPictureUnavailable(") + function(late, "uint64_t BeginStreamlinePicture(") + function(late, "void EndStreamlinePicture("))
encode = function(late, "static void RecordPqEncode(")
write("FinishedPqEncode.inl", encode)
encode, hits = re.subn(r"cmd->ResourceBarrier\(1, &order\);", "ObserveUavBarrier(cmd, 1, &order);", encode)
if hits != 1:
    raise RuntimeError("Production same-state UAV ordering instrumentation did not match")
write("FinishedPqEncodeObserved.inl", encode)


base_methods = [
    "DXGI_FORMAT Shader_Dx12::TranslateTypelessFormats(",
    "bool Shader_Dx12::CreateComputeShader(",
    "bool Shader_Dx12::CreateComputePipeline(",
    "void Shader_Dx12::CreateShaderResourceView(",
    "void Shader_Dx12::CreateUnorderedAccessView(",
    "bool Shader_Dx12::SetupRootSignature(",
    "bool Shader_Dx12::InitHeaps(",
]
write("FinishedShaderMethods.inl", "\n".join(function(shader, s).replace("Shader_Dx12::", "ProductionBase::")
                                             for s in base_methods))
methods = ["bool DlssNr_Dx12::RecordFinishedPqRaster(", "bool DlssNr_Dx12::FinishedPqDirectReady(",
           "bool DlssNr_Dx12::DispatchResidualPass("]
write("FinishedComposeMethods.inl", "\n".join(function(compose, s).replace("DlssNr_Dx12::", "ProductionCompose::")
                                              for s in methods))
# Use the actual constructor's root, constants and descriptor-ring setup. The unrelated main NR
# shader is deliberately not created; these tests never run NR inference or its main shader.
ctor = function(compose, "DlssNr_Dx12::DlssNr_Dx12(")
ctor = ctor.replace("DlssNr_Dx12::DlssNr_Dx12", "ProductionCompose::ProductionCompose")
ctor = ctor.replace("Shader_Dx12(InName, InDevice)", "ProductionBase(InName, InDevice)")
pattern = r"    if \(!CreateComputePipeline\(InDevice, &_pipelineState, DlssNr_cso,[\s\S]*?\n    }\n"
ctor, hits = re.subn(pattern, "", ctor)
if hits != 1:
    raise RuntimeError("Main NR constructor exclusion did not match exactly once")
write("FinishedComposeConstructor.inl", ctor)

# Resource upload/readback helpers are shared with the existing performance fixture, not copied.
fixture = read("tests/nr_gpu/nr_finished_performance.cpp")
start, end = fixture.index("static D3D12_RESOURCE_DESC texture("), fixture.index("struct Timing\n")
if end <= start:
    raise RuntimeError("Finished image fixture markers changed")
write("FinishedTestImages.inl", fixture[start:end])

# A seam may not accidentally replace the guarded production call sites.
for source, needles in [
    (hooks, ["FinishedPicturePluginFunction(functionName, LoadOriginal(o_dlssg_slGetPluginFunction), false)",
             "FinishedPicturePluginFunction(functionName, LoadOriginal(o_local_dlssg_slGetPluginFunction), true)"]),
    (native, ["StreamlinePicture::Owns(swapChain)", "if (DlssNr::StreamlinePicture::NativeFgRunning())"]),
    (late, ["const auto pick = SelectFinishedInput(", "if (StreamlinePicture::NativeFgRunning())"]),
]:
    for needle in needles:
        if needle not in source:
            raise RuntimeError(f"Production entry no longer uses tested policy: {needle}")

header = read("OptiScaler/shaders/dlssnr/DlssNr_Dx12.h")
for declaration in ["#define DLSSNR_NUM_OF_HEAPS 96", "kSrvCount = 6", "kUavCount = 2", "_numThreadsX = 8", "_numThreadsY = 8"]:
    if declaration not in header:
        raise RuntimeError(f"Production PQ owning-class layout changed: {declaration}")


start = compose.index("    if (finishedMode)\n")
end = compose.index("\n    Late::Cancel();", start)
if end <= start or "StreamlinePicture::PauseEvaluation()" not in compose[start:end]:
    raise RuntimeError("Finished evaluate pause missing/moved")
write("FinishedEvaluate.inl", "void Evaluate()\n{\n" + compose[start:end] + "\n}\n")
acquire = function(late, "Slot* Acquire(")
end = acquire.index("    ComPtr<ID3D12Device> currentDevice;")
if "StreamlinePicture::PauseEvaluation()" not in acquire[:end]:
    raise RuntimeError("Capture acquire pause missing/moved")
write("FinishedAcquire.inl", acquire[:end] + "    return &testSlot; // no device/NGX work in this harness\n}\n")
resume = function(read("OptiScaler/native/NativeDriver.cpp"), "NativeDriver::RunResult NativeDriver::RunFrame(")
prefix = resume[resume.index("    // FG has stopped."):resume.index("    FrameInput input;")]
write("FinishedNativeResume.inl", "void Driver::ResumeForTest()\n{\n" + prefix + "}\n")

print("Prepared production Finished Picture wrappers, selection, shader helpers and constructor")
