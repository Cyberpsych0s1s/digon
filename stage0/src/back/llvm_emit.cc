#include "llvm_emit.h"

#include <string>

#include "map.h"
#include "vec.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

namespace {

// Emit-time layout for a payload-bearing enum.
//   repr  - named LLVM struct { i32 tag, [size x i8] buf }
//   align - alignment for allocas / loads / stores of this repr
struct EnumLayout {
    llvm::StructType* repr;
    uint64_t          align;
};

llvm::Type* map_type(llvm::LLVMContext& ctx,
                     const Vec<llvm::StructType*>& structTys,
                     const Vec<EnumLayout>& enumLays,
                     MType t) {
    switch (t.kind) {
        case MTypeKind::Void:  return llvm::Type::getVoidTy(ctx);
        case MTypeKind::Bool:  return llvm::Type::getInt1Ty(ctx);
        case MTypeKind::Int:   return llvm::Type::getIntNTy(ctx, t.bits);
        case MTypeKind::Float: return t.bits == 32 ? llvm::Type::getFloatTy(ctx)
                                                   : llvm::Type::getDoubleTy(ctx);
        case MTypeKind::Ptr:    return llvm::PointerType::getUnqual(ctx);
        case MTypeKind::Struct: return structTys[t.struct_index];
        case MTypeKind::Ref:    return llvm::PointerType::getUnqual(ctx);
        case MTypeKind::Enum:   return enumLays[t.struct_index].repr;
    }
    return llvm::Type::getVoidTy(ctx);
}

void report(Diag* diag, const char* msg) {
    diag_error(diag, Span{0, 0, 0}, msg);
}

} // namespace

bool emit_object(const MModule* mod, const Interner* in, const char* obj_path,
                 OptLevel opt, Diag* diag) {
    using namespace llvm;

    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();

    LLVMContext ctx;
    // Qualify llvm::Module explicitly: our AST also has a `Module` type.
    auto        module = std::make_unique<llvm::Module>("digon", ctx);
    IRBuilder<> builder(ctx);

    Type* i32 = Type::getInt32Ty(ctx);

    // The target's DataLayout is needed up front to size mixed-payload enum
    // repr structs. The host target's TM is reused below.
    std::string    tripleStr = sys::getDefaultTargetTriple();
    Triple         triple(tripleStr);
    std::string    targetErr;
    const Target*  target = TargetRegistry::lookupTarget(tripleStr, targetErr);
    if (!target) {
        report(diag, targetErr.c_str());
        return false;
    }
    TargetOptions   targetOpts;
    CodeGenOptLevel cgOpt = (opt == OptLevel::O2) ? CodeGenOptLevel::Aggressive
                                                  : CodeGenOptLevel::None;
    TargetMachine*  tm = target->createTargetMachine(
        triple, "generic", "", targetOpts, std::optional<Reloc::Model>(),
        std::optional<CodeModel::Model>(), cgOpt);
    DataLayout dl = tm->createDataLayout();

    // Create all named struct types opaque first so fields can reference any
    // struct, then set their bodies. Enum repr structs are likewise created
    // opaque now; their bodies are set after struct bodies (payloads may name
    // structs), once payload sizes are known.
    Vec<StructType*> structTys{};
    for (uint32_t i = 0; i < mod->nstructs; i++) {
        size_t      nlen = 0;
        const char* nm   = intern_str(in, mod->structs[i].name, &nlen);
        structTys.push(StructType::create(ctx, StringRef(nm ? nm : "", nlen)));
    }
    Vec<EnumLayout> enumLays{};
    for (uint32_t i = 0; i < mod->nenums; i++) {
        size_t      nlen = 0;
        const char* nm   = intern_str(in, mod->enums[i].name, &nlen);
        EnumLayout  el{};
        el.repr  = StructType::create(ctx, StringRef(nm ? nm : "", nlen));
        el.align = 1;
        enumLays.push(el);
    }
    for (uint32_t i = 0; i < mod->nstructs; i++) {
        const MStructDef& sd = mod->structs[i];
        Vec<Type*> ftys{};
        for (uint32_t k = 0; k < sd.nfields; k++)
            ftys.push(map_type(ctx, structTys, enumLays, sd.field_types[k]));
        structTys[i]->setBody(ArrayRef<Type*>(ftys.data, ftys.len));
        ftys.free();
    }
    // With struct bodies set, query DataLayout for variant payload
    // sizes/alignments and finalize each enum's repr.
    for (uint32_t i = 0; i < mod->nenums; i++) {
        const MEnumDef& ed = mod->enums[i];
        uint64_t maxSize  = 0;
        uint64_t maxAlign = 1;
        for (uint32_t k = 0; k < ed.nvariants; k++) {
            if (ed.variant_payloads[k].kind == MTypeKind::Void) continue;
            Type*    pty   = map_type(ctx, structTys, enumLays, ed.variant_payloads[k]);
            uint64_t sz    = dl.getTypeAllocSize(pty);
            uint64_t align = dl.getABITypeAlign(pty).value();
            if (sz    > maxSize)  maxSize  = sz;
            if (align > maxAlign) maxAlign = align;
        }
        // Round up size to alignment so an i8 buffer of that length keeps the
        // payload's natural alignment relative to the start of the repr.
        if (maxAlign > 1 && (maxSize & (maxAlign - 1))) {
            maxSize = (maxSize + maxAlign - 1) & ~(maxAlign - 1);
        }
        if (maxSize == 0) maxSize = 1; // at least one byte so GEP is well-typed
        Type* bufTy = ArrayType::get(Type::getInt8Ty(ctx), maxSize);
        Type* fields[2] = { Type::getInt32Ty(ctx), bufTy };
        enumLays[i].repr->setBody(ArrayRef<Type*>(fields, 2));
        enumLays[i].align = maxAlign;
    }

    // Pass A: declare every function so calls (including forward refs) resolve.
    Vec<Function*> fns{};
    for (uint32_t i = 0; i < mod->nfuncs; i++) {
        const MFunc& mf = mod->funcs[i];

        Type* retTy = mf.is_main ? i32 : map_type(ctx, structTys, enumLays, mf.ret);
        Vec<Type*> params{};
        for (uint32_t k = 0; k < mf.nparams; k++) params.push(map_type(ctx, structTys, enumLays, mf.param_types[k]));

        FunctionType* fnTy = FunctionType::get(
            retTy, ArrayRef<Type*>(params.data, params.len), mf.variadic);
        params.free();

        size_t      nlen = 0;
        const char* name = intern_str(in, mf.name, &nlen);
        Function*   fn   = Function::Create(fnTy, Function::ExternalLinkage,
                                            StringRef(name, nlen), module.get());
        fns.push(fn);
    }

    // Cache of emitted C-string globals, keyed by interned id.
    Map str_globals{};

    // Pass B: emit bodies.
    for (uint32_t i = 0; i < mod->nfuncs; i++) {
        const MFunc& mf = mod->funcs[i];
        if (mf.is_extern || mf.ninsts == 0) continue;

        Function* fn = fns[i];

        // One LLVM basic block per mid-IR block; create them all up front so
        // branch targets resolve.
        Vec<BasicBlock*> bbs{};
        for (uint32_t b = 0; b < mf.nblocks; b++)
            bbs.push(BasicBlock::Create(ctx, "bb", fn));

        Vec<Value*> vals{};
        vals.reserve(mf.ninsts);
        for (uint32_t k = 0; k < mf.ninsts; k++) vals.push(nullptr);

        for (uint32_t b = 0; b < mf.nblocks; b++) {
            builder.SetInsertPoint(bbs[b]);
            const MBlock& blk = mf.blocks[b];
            for (uint32_t j = 0; j < blk.nids; j++) {
            uint32_t     k   = blk.inst_ids[j];
            const MInst& ins = mf.insts[k];
            Value*       out = nullptr;
            switch (ins.op) {
                case MOp::ConstInt:
                    if (ins.type.kind == MTypeKind::Ptr)
                        out = ConstantPointerNull::get(PointerType::getUnqual(ctx));
                    else
                        out = ConstantInt::get(map_type(ctx, structTys, enumLays, ins.type), ins.imm_int);
                    break;
                case MOp::ConstFloat:
                    out = ConstantFP::get(map_type(ctx, structTys, enumLays, ins.type), ins.imm_float);
                    break;
                case MOp::ConstBool:
                    out = ConstantInt::get(Type::getInt1Ty(ctx), ins.imm_bool ? 1 : 0);
                    break;
                case MOp::ConstCStr: {
                    void* cached = nullptr;
                    if (map_lookup(&str_globals, ins.str_id, &cached)) {
                        out = static_cast<Value*>(cached);
                    } else {
                        size_t      slen = 0;
                        const char* s    = intern_str(in, ins.str_id, &slen);
                        out = builder.CreateGlobalString(StringRef(s ? s : "", slen));
                        map_put(&str_globals, ins.str_id, out);
                    }
                    break;
                }
                case MOp::Param:
                    out = fn->getArg(ins.param_index);
                    break;
                case MOp::Alloca:
                    out = builder.CreateAlloca(map_type(ctx, structTys, enumLays, ins.type));
                    break;
                case MOp::Load:
                    out = builder.CreateLoad(map_type(ctx, structTys, enumLays, ins.type), vals[ins.a]);
                    break;
                case MOp::Store:
                    builder.CreateStore(vals[ins.b], vals[ins.a]);
                    break;
                case MOp::Add:  out = builder.CreateAdd(vals[ins.a], vals[ins.b]); break;
                case MOp::Sub:  out = builder.CreateSub(vals[ins.a], vals[ins.b]); break;
                case MOp::Mul:  out = builder.CreateMul(vals[ins.a], vals[ins.b]); break;
                case MOp::SDiv: out = builder.CreateSDiv(vals[ins.a], vals[ins.b]); break;
                case MOp::UDiv: out = builder.CreateUDiv(vals[ins.a], vals[ins.b]); break;
                case MOp::SRem: out = builder.CreateSRem(vals[ins.a], vals[ins.b]); break;
                case MOp::URem: out = builder.CreateURem(vals[ins.a], vals[ins.b]); break;
                case MOp::And:  out = builder.CreateAnd(vals[ins.a], vals[ins.b]); break;
                case MOp::Or:   out = builder.CreateOr(vals[ins.a], vals[ins.b]); break;
                case MOp::Xor:  out = builder.CreateXor(vals[ins.a], vals[ins.b]); break;
                case MOp::Shl:  out = builder.CreateShl(vals[ins.a], vals[ins.b]); break;
                case MOp::AShr: out = builder.CreateAShr(vals[ins.a], vals[ins.b]); break;
                case MOp::LShr: out = builder.CreateLShr(vals[ins.a], vals[ins.b]); break;
                case MOp::FAdd: out = builder.CreateFAdd(vals[ins.a], vals[ins.b]); break;
                case MOp::FSub: out = builder.CreateFSub(vals[ins.a], vals[ins.b]); break;
                case MOp::FMul: out = builder.CreateFMul(vals[ins.a], vals[ins.b]); break;
                case MOp::FDiv: out = builder.CreateFDiv(vals[ins.a], vals[ins.b]); break;
                case MOp::ICmpEq: out = builder.CreateICmpEQ(vals[ins.a], vals[ins.b]); break;
                case MOp::ICmpNe: out = builder.CreateICmpNE(vals[ins.a], vals[ins.b]); break;
                case MOp::ICmpLt: out = builder.CreateICmpSLT(vals[ins.a], vals[ins.b]); break;
                case MOp::ICmpGt: out = builder.CreateICmpSGT(vals[ins.a], vals[ins.b]); break;
                case MOp::ICmpLe: out = builder.CreateICmpSLE(vals[ins.a], vals[ins.b]); break;
                case MOp::ICmpGe: out = builder.CreateICmpSGE(vals[ins.a], vals[ins.b]); break;
                case MOp::ICmpULt: out = builder.CreateICmpULT(vals[ins.a], vals[ins.b]); break;
                case MOp::ICmpUGt: out = builder.CreateICmpUGT(vals[ins.a], vals[ins.b]); break;
                case MOp::ICmpULe: out = builder.CreateICmpULE(vals[ins.a], vals[ins.b]); break;
                case MOp::ICmpUGe: out = builder.CreateICmpUGE(vals[ins.a], vals[ins.b]); break;
                case MOp::FCmpEq: out = builder.CreateFCmpOEQ(vals[ins.a], vals[ins.b]); break;
                case MOp::FCmpNe: out = builder.CreateFCmpONE(vals[ins.a], vals[ins.b]); break;
                case MOp::FCmpLt: out = builder.CreateFCmpOLT(vals[ins.a], vals[ins.b]); break;
                case MOp::FCmpGt: out = builder.CreateFCmpOGT(vals[ins.a], vals[ins.b]); break;
                case MOp::FCmpLe: out = builder.CreateFCmpOLE(vals[ins.a], vals[ins.b]); break;
                case MOp::FCmpGe: out = builder.CreateFCmpOGE(vals[ins.a], vals[ins.b]); break;
                case MOp::INeg: out = builder.CreateNeg(vals[ins.a]); break;
                case MOp::FNeg: out = builder.CreateFNeg(vals[ins.a]); break;
                case MOp::Not:  out = builder.CreateNot(vals[ins.a]); break;
                case MOp::Trunc:   out = builder.CreateTrunc(vals[ins.a], map_type(ctx, structTys, enumLays, ins.type)); break;
                case MOp::SExt:    out = builder.CreateSExt(vals[ins.a], map_type(ctx, structTys, enumLays, ins.type)); break;
                case MOp::ZExt:    out = builder.CreateZExt(vals[ins.a], map_type(ctx, structTys, enumLays, ins.type)); break;
                case MOp::FpTrunc: out = builder.CreateFPTrunc(vals[ins.a], map_type(ctx, structTys, enumLays, ins.type)); break;
                case MOp::FpExt:   out = builder.CreateFPExt(vals[ins.a], map_type(ctx, structTys, enumLays, ins.type)); break;
                case MOp::SiToFp:  out = builder.CreateSIToFP(vals[ins.a], map_type(ctx, structTys, enumLays, ins.type)); break;
                case MOp::UiToFp:  out = builder.CreateUIToFP(vals[ins.a], map_type(ctx, structTys, enumLays, ins.type)); break;
                case MOp::FpToSi:  out = builder.CreateFPToSI(vals[ins.a], map_type(ctx, structTys, enumLays, ins.type)); break;
                case MOp::FpToUi:  out = builder.CreateFPToUI(vals[ins.a], map_type(ctx, structTys, enumLays, ins.type)); break;
                case MOp::SignCast:
                    // Same-width signedness change: LLVM integers are signless,
                    // so this is a pure MType rebrand with no instruction.
                    out = vals[ins.a];
                    break;
                case MOp::MakeStruct: {
                    StructType* sty = structTys[ins.type.struct_index];
                    Value*      agg = UndefValue::get(sty);
                    for (uint32_t ai = 0; ai < ins.nargs; ai++)
                        agg = builder.CreateInsertValue(agg, vals[ins.args[ai]], {ai});
                    out = agg;
                    break;
                }
                case MOp::GetField:
                    out = builder.CreateExtractValue(vals[ins.a], {ins.field_index});
                    break;
                case MOp::FieldPtr: {
                    StructType* st = structTys[static_cast<uint32_t>(ins.imm_int)];
                    out = builder.CreateStructGEP(st, vals[ins.a], ins.field_index);
                    break;
                }
                case MOp::EnumCons: {
                    // Stack-build a fresh enum repr, populate tag + payload,
                    // then load it back as a first-class value.
                    const EnumLayout& el  = enumLays[ins.type.struct_index];
                    AllocaInst*       slot = builder.CreateAlloca(el.repr);
                    slot->setAlignment(Align(el.align));
                    Value* tagPtr = builder.CreateStructGEP(el.repr, slot, 0);
                    builder.CreateStore(ConstantInt::get(Type::getInt32Ty(ctx),
                                                        ins.imm_int), tagPtr);
                    if (ins.a != M_NO_VALUE) {
                        // The payload field is `[N x i8]`; with opaque pointers
                        // the same GEP value re-types to the payload's struct.
                        Value* bufPtr = builder.CreateStructGEP(el.repr, slot, 1);
                        StoreInst* st = builder.CreateStore(vals[ins.a], bufPtr);
                        st->setAlignment(Align(el.align));
                    }
                    LoadInst* ld = builder.CreateLoad(el.repr, slot);
                    ld->setAlignment(Align(el.align));
                    out = ld;
                    break;
                }
                case MOp::EnumTag:
                    out = builder.CreateExtractValue(vals[ins.a], {0});
                    break;
                case MOp::EnumPayload: {
                    // The payload lives in the `[N x i8]` buffer; spill the
                    // enum back to memory and load the typed payload from there.
                    StructType* reprTy = cast<StructType>(vals[ins.a]->getType());
                    // Look up the enum's alignment by matching its repr type.
                    uint64_t align = 1;
                    for (uint32_t k = 0; k < enumLays.len; k++) {
                        if (enumLays[k].repr == reprTy) { align = enumLays[k].align; break; }
                    }
                    AllocaInst* slot = builder.CreateAlloca(reprTy);
                    slot->setAlignment(Align(align));
                    StoreInst*  st   = builder.CreateStore(vals[ins.a], slot);
                    st->setAlignment(Align(align));
                    Value*      bufP = builder.CreateStructGEP(reprTy, slot, 1);
                    Type*       payTy = map_type(ctx, structTys, enumLays, ins.type);
                    LoadInst*   ld   = builder.CreateLoad(payTy, bufP);
                    ld->setAlignment(Align(align));
                    out = ld;
                    break;
                }
                case MOp::AddrOf:
                    // Rebrand the alloca's pointer with a Ref MType — no LLVM
                    // instruction needed; the alloca's value is already a ptr.
                    out = vals[ins.a];
                    break;
                case MOp::Call: {
                    Function*   callee = fns[ins.callee];
                    Vec<Value*> args{};
                    for (uint32_t ai = 0; ai < ins.nargs; ai++) args.push(vals[ins.args[ai]]);
                    out = builder.CreateCall(callee, ArrayRef<Value*>(args.data, args.len));
                    args.free();
                    break;
                }
                case MOp::Br:
                    builder.CreateBr(bbs[ins.target]);
                    break;
                case MOp::CondBr:
                    builder.CreateCondBr(vals[ins.a], bbs[ins.target], bbs[ins.target2]);
                    break;
                case MOp::Ret:         builder.CreateRet(vals[ins.a]); break;
                case MOp::RetVoid:     builder.CreateRetVoid(); break;
                case MOp::Unreachable: builder.CreateUnreachable(); break;
            }
            vals[k] = out;
            }
        }
        vals.free();
        bbs.free();

        std::string              verr;
        raw_string_ostream       vos(verr);
        if (verifyFunction(*fn, &vos)) {
            report(diag, ("internal codegen error: " + verr).c_str());
            fns.free();
            map_free(&str_globals);
            return false;
        }
    }
    fns.free();
    map_free(&str_globals);

    // Apply the DataLayout (from the up-front TargetMachine, used earlier for
    // enum repr sizing) now that all functions are emitted.
    module->setDataLayout(dl);
    module->setTargetTriple(triple);

    std::string vmerr;
    raw_string_ostream vmos(vmerr);
    if (verifyModule(*module, &vmos)) {
        report(diag, ("internal codegen error (module): " + vmerr).c_str());
        delete tm;
        return false;
    }

    std::error_code   ec;
    raw_fd_ostream    dest(obj_path, ec, sys::fs::OF_None);
    if (ec) {
        report(diag, ("could not open object file: " + ec.message()).c_str());
        delete tm;
        return false;
    }

    legacy::PassManager pass;
    if (tm->addPassesToEmitFile(pass, dest, nullptr, CodeGenFileType::ObjectFile)) {
        report(diag, "target cannot emit an object file");
        delete tm;
        return false;
    }
    pass.run(*module);
    dest.flush();

    structTys.free();
    enumLays.free();
    delete tm;
    return true;
}
