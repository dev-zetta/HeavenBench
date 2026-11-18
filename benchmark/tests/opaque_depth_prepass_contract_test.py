#!/usr/bin/env python3
"""Execute the actual portable SPIR-V replay classifier; no GPU or GPU libraries.

Small SPIR-V1.3 fixtures cover shader admission, not native raster equivalence.
Malformed streams are separate policy/memory bounds probes. --work-dir retains
all generated inputs, compiler output and falsifiers for investigation.
"""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

# Standard SPIR-V instruction numbers; fixtures do not read classifier predicates.
OP = dict(Cap=17, Memory=14, Entry=15, Mode=16, Decorate=71, Member=72,
          Void=19, Bool=20, Int=21, Float=22, Vector=23, Array=28, Struct=30,
          Pointer=32, FunctionType=33, Constant=43, Composite=44, False_=42,
          Variable=59, Function=54, End=56, Label=248, Store=62, Access=65,
          Merge=247, Branch=250, Kill=252, Return=253)


def ins(name, *values):
    return [((len(values)+1) << 16) | OP[name], *values]


def string(value):
    raw=value.encode()+b"\0"; raw+=b"\0"*((-len(raw))%4)
    return list(struct.unpack("<"+"I"*(len(raw)//4),raw))


def module(vertex=False, invariant=True, kill=False, clip=False, block=False, external=False, width=4):
    annotations=[]; extra_types=[]; variables=[]; operations=[]
    entry=[42] if block else [20]
    if block:
        annotations+=ins("Decorate",40,2)+ins("Member",40,0,11,0)+ins("Member",40,0,18)
        annotations+=ins("Member",40,1,11,3) # An unused gl_PerVertex clip member is legal.
        extra_types+=ins("Int",18,32,0)+ins("Constant",18,19,0)+ins("Constant",18,23,2)
        extra_types+=ins("Array",22,3,23)+ins("Struct",40,4,22)+ins("Pointer",41,3,40)
        variables+=ins("Variable",41,42,3); operations+=ins("Access",5,45,42,19)+ins("Store",45,7)
    else:
        annotations+=ins("Decorate",20,11,0) if vertex else ins("Decorate",20,30,0)
        if vertex and invariant: annotations+=ins("Decorate",20,18)
        variables+=ins("Variable",5,20,3); operations+=ins("Store",20,7)
    if clip:
        annotations+=ins("Decorate",21,11,3)
        extra_types+=ins("Int",18,32,0)+ins("Constant",18,23,2)+ins("Array",22,3,23)
        extra_types+=ins("Pointer",24,3,22)+ins("Composite",22,25,6,6)
        variables+=ins("Variable",24,21,3); entry.append(21); operations+=ins("Store",21,25)
    if external:
        annotations+=ins("Decorate",30,2)+ins("Member",30,0,35,0)
        annotations+=ins("Decorate",32,34,0)+ins("Decorate",32,33,0)
        extra_types+=ins("Int",18,32,0)+ins("Constant",18,19,0)+ins("Struct",30,3)
        extra_types+=ins("Pointer",31,12,30)+ins("Pointer",33,12,3)
        variables+=ins("Variable",31,32,12); operations+=ins("Access",33,34,32,19)+ins("Store",34,6)
    if kill:
        extra_types+=ins("Bool",8)+ins("False_",8,9)
        operations=ins("Merge",36,0)+ins("Branch",9,35,36)+ins("Label",35)+ins("Kill")+ins("Label",36)+operations
    return ([0x07230203,0x00010300,0,64,0]+ins("Cap",1)+(ins("Cap",32) if clip else [])+
            ins("Memory",0,1)+ins("Entry",0 if vertex else 4,10,*string("main"),*entry)+
            ([] if vertex else ins("Mode",10,7))+annotations+
            ins("Void",1)+ins("FunctionType",2,1)+ins("Float",3,32)+ins("Vector",4,3,width)+
            ins("Pointer",5,3,4)+ins("Constant",3,6,0x3f800000)+ins("Composite",4,7,*([6]*width))+
            extra_types+variables+ins("Function",1,10,0,2)+ins("Label",11)+operations+ins("Return")+ins("End"))


RUNNER = r'''
#include "OpaqueDepthPrepassSafety.h"
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
unsigned classify(std::uint32_t const* w,std::size_t n) {
    using namespace filament::opaque_depth;
    return unsigned(isSafeOpaqueDepthPrepassFragment(w,n)) |
        unsigned(isDepthRejectionSafeFragment(w,n))*2 |
        unsigned(isDepthPrepassVertexReplaySafe(w,n))*4;
}
int main(int argc,char**argv) {
    if(argc!=2)return 2;
    std::uint32_t header[]={0x07230203u,0x00010300u,0,64,0};
    if(classify(nullptr,0)||classify(header,262145))return 1;
    std::ifstream list(argv[1]);if(!list)return 2;std::string line;unsigned cases=2,failed=0;
    while(std::getline(list,line)) {
        auto split=line.find('\t');if(split==std::string::npos)return 2;
        auto expected=unsigned(std::stoul(line.substr(0,split)));auto path=line.substr(split+1);
        std::ifstream in(path,std::ios::binary|std::ios::ate);if(!in)return 2;
        auto offset=in.tellg();if(offset<0||offset>1048576||offset%4)return 2;
        auto size=static_cast<std::streamsize>(offset);
        std::vector<std::uint32_t>w(static_cast<std::size_t>(size)/4);in.seekg(0);
        if(size)in.read(reinterpret_cast<char*>(w.data()),size);
        if(!in)return 2;
        auto actual=classify(w.data(),w.size());++cases;
        if(actual!=expected){++failed;std::cerr<<path<<": actual "<<actual<<" expected "<<expected<<'\n';}
    }
    std::cout<<cases<<" shader/bounds checks, "<<failed<<" failures\n";return failed?1:0;
}
'''


def execute(args, work):
    header=args.filament/"filament/src/OpaqueDepthPrepassSafety.h"
    original=header.read_text(); runner=work/"runner.cpp"; runner.write_text(RUNNER)
    base=module(); cases=[("fragment",base,3),("discard",module(kill=True),2),
        ("invariant-position",module(vertex=True),4),("noninvariant-position",module(vertex=True,invariant=False),0),
        ("unused-clip-member",module(vertex=True,block=True),4),("active-clip-distance",module(vertex=True,clip=True),0),
        ("external-store",module(external=True),0),("vec3-color-output",module(width=3),0)]
    unknown=list(base);unknown[-1:-1]=[(1<<16)|65535];cases.append(("unknown-opcode",unknown,0))
    huge=list(base);huge[3]=65537;cases.append(("excessive-id-bound",huge,0))
    for length in range(len(base)):cases.append(("truncated-"+str(length),base[:length],0))
    manifest=work/"cases.tsv"
    with manifest.open("x") as table:
        for label,words,expected in cases:
            path=work/(label+".spv");path.write_bytes(struct.pack("<"+"I"*len(words),*words))
            table.write(f"{expected}\t{path}\n")
    microsoft=Path(args.cxx).stem.lower() in ("cl","clang-cl")
    def run(label, content, expected):
        folder=work/label;folder.mkdir();(folder/header.name).write_text(content)
        exe=folder/("test.exe" if microsoft else "test")
        flags=["/nologo","/std:c++17","/EHsc","/W4","/WX","/I"+str(folder),str(runner),"/Fe:"+str(exe)] if microsoft else ["-std=c++17","-O0","-Wall","-Wextra","-Werror","-I",str(folder),str(runner),"-o",str(exe)]
        built=subprocess.run([args.cxx,*flags],cwd=folder,capture_output=True,text=True,timeout=20)
        (folder/"compile.log").write_text(built.stdout+built.stderr)
        if built.returncode:raise RuntimeError("C++ compile failed: "+str(folder/"compile.log")+"\n"+built.stderr)
        result=subprocess.run([str(exe),str(manifest)],capture_output=True,text=True,timeout=10)
        (folder/"run.log").write_text(result.stdout+result.stderr)
        if result.returncode!=expected:raise RuntimeError(label+" unexpected result\n"+result.stdout+result.stderr)
    run("actual",original,0)
    stub="#include <cstddef>\n#include <cstdint>\nnamespace filament::opaque_depth {\n"
    for value in ("true","false"):
        functions="".join("inline bool "+name+"(std::uint32_t const*,std::size_t) noexcept{return "+value+";}\n" for name in ("isSafeOpaqueDepthPrepassFragment","isDepthRejectionSafeFragment","isDepthPrepassVertexReplaySafe"))
        run("always-"+value,stub+functions+"}\n",1)
    weakened=original.replace("if (vertex && !invariantVertexPosition(ids.get(), bound)) return false;","// missing required Position invariance",1)
    if weakened==original:raise RuntimeError("required invariance negative-control seam changed; review test")
    run("missing-invariance",weakened,1)
    weakened=original.replace("if (count != 2 || (w[1] != 0 && w[1] != 1 && w[1] != 50)) return false;",
        "if (count != 2 || (w[1] != 0 && w[1] != 1 && w[1] != 50 && !(vertex && w[1] == 32))) return false;",1)
    if weakened==original:raise RuntimeError("ClipDistance negative-control seam changed; review test")
    run("missing-clip-distance-gate",weakened,1)
    print(json.dumps(dict(passed=True,cases=len(cases)+2,negative_controls=4,scope="Portable actual-header CPU policy; no native raster, package or performance acceptance.")))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--filament",type=Path,default=Path(__file__).resolve().parents[2]/"lib/filament")
    parser.add_argument("--cxx",default=shutil.which("clang++") or shutil.which("g++") or shutil.which("cl"))
    parser.add_argument("--work-dir",type=Path);args=parser.parse_args()
    if not args.cxx:parser.error("a host C++17 compiler is required")
    args.cxx=shutil.which(args.cxx) or args.cxx;args.filament=args.filament.resolve()
    if args.work_dir:
        work=args.work_dir.resolve();work.mkdir(parents=True,exist_ok=False);execute(args,work)
    else:
        with tempfile.TemporaryDirectory(prefix="heaven-opaque-depth-contract-") as work:execute(args,Path(work))


if __name__=="__main__":main()
