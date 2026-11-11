#!/usr/bin/env python3
"""Build original BPF tools for the two glibc guest ABIs, without host headers."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[3]
BASE = ROOT/'build/bpf-20261107'
LINUX_REVISION = 'f4cdf7ca9a1fdcca413157df19753f388a5a224e'
ARCHIVES = {name:'bpf/'+name for name in (
    'bpftrace-0.27.1.tar.gz', 'libbpf-85c0d575.tar.gz',
    'llvm-project-23.1.4.src.tar.xz', 'bcc-0.35.0.tar.gz',
    'cereal-1.3.2.tar.gz', 'elfutils-0.194.tar.bz2', 'zlib-1.3.2.tar.gz')}
TARGETS = {
 'riscv': ('riscv64', '/usr/bin/riscv64-linux-gnu-', 'riscv64-unknown-linux-gnu'),
 'loongarch': ('loongarch64', '/opt/loongarch64-tools/bin/loongarch64-unknown-linux-gnu-', 'loongarch64-unknown-linux-gnu'),
}


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def references():
    rows = {}
    for line in (ROOT/'references/sources.tsv').read_text().splitlines():
        if not line or line.startswith('#'): continue
        row = line.split('\t')
        if len(row) != 5 or row[1] in rows:
            raise RuntimeError('invalid or duplicate reference identity: '+line)
        rows[row[1]] = row
    return rows


def verify_archive(path, expected):
    if sha(path) != expected:
        raise RuntimeError(str(path)+': SHA-256 differs from references/sources.tsv')


def source(name):
    row = references()[ARCHIVES[name]]
    archive = BASE/'downloads'/name
    if not archive.is_file():
        reference = ROOT/'references'/row[1]
        archive.parent.mkdir(parents=True, exist_ok=True)
        if reference.is_file(): shutil.copyfile(reference, archive)
        else:
            temporary = archive.with_suffix(archive.suffix+'.part')
            run(['curl','-fL','--retry','3','--retry-all-errors',row[2],'-o',temporary], BASE/('fetch-'+name+'.log'))
            verify_archive(temporary, row[4])
            temporary.rename(archive)
    verify_archive(archive, row[4])
    with tarfile.open(archive) as stream:
        top = stream.getnames()[0].split('/')[0]
    target = BASE/'src'/top
    if not target.is_dir():
        target.parent.mkdir(parents=True, exist_ok=True)
        run(['tar','-xf',archive,'-C',target.parent], BASE/('extract-'+name+'.log'))
    return target


def run(command, log, *, cwd=None, env=None):
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w') as stream:
        subprocess.run(list(map(str, command)), cwd=cwd, env=env,
                       stdout=stream, stderr=subprocess.STDOUT, check=True)


def environment(arch):
    result = os.environ.copy()
    for name in ('CPATH','C_INCLUDE_PATH','CPLUS_INCLUDE_PATH','LIBRARY_PATH','LD_LIBRARY_PATH','MAKEFLAGS','MFLAGS'):
        result.pop(name, None)
    prefix = BASE/arch/'root/usr'
    result.update(CC=TARGETS[arch][1]+'gcc', CXX=TARGETS[arch][1]+'g++',
                  AR=TARGETS[arch][1]+'ar', RANLIB=TARGETS[arch][1]+'ranlib',
                  PKG_CONFIG_LIBDIR=str(prefix/'lib/pkgconfig'),
                  CPPFLAGS='-I'+str(prefix/'include'), LDFLAGS='-L'+str(prefix/'lib'),
                  GIT_CEILING_DIRECTORIES=str(BASE),
                  LC_ALL='C', TZ='UTC')
    return result


def llvm_options(arch):
    machine, compiler, triple = TARGETS[arch]
    flags = dict(CMAKE_BUILD_TYPE='Release', CMAKE_SYSTEM_NAME='Linux',
        CMAKE_SYSTEM_PROCESSOR=machine, CMAKE_C_COMPILER=compiler+'gcc',
        CMAKE_CXX_COMPILER=compiler+'g++', CMAKE_INSTALL_PREFIX=BASE/arch/'root/usr',
        LLVM_HOST_TRIPLE=triple, LLVM_DEFAULT_TARGET_TRIPLE=triple,
        LLVM_TARGETS_TO_BUILD='BPF', LLVM_ENABLE_PROJECTS='clang',
        LLVM_ENABLE_ZLIB='OFF', LLVM_ENABLE_ZSTD='OFF', LLVM_ENABLE_LIBXML2='OFF',
        LLVM_ENABLE_LIBEDIT='OFF', LLVM_ENABLE_FFI='OFF', LLVM_ENABLE_RTTI='ON',
        LLVM_ENABLE_ASSERTIONS='OFF', LLVM_INCLUDE_TESTS='OFF', LLVM_INCLUDE_BENCHMARKS='OFF',
        LLVM_INCLUDE_EXAMPLES='OFF', LLVM_TABLEGEN='/usr/bin/llvm-tblgen',
        CLANG_TABLEGEN='/usr/bin/clang-tblgen', LLVM_NATIVE_TOOL_DIR='/usr/bin',
        LLVM_BUILD_TOOLS='OFF', LLVM_BUILD_LLVM_DYLIB='ON', LLVM_LINK_LLVM_DYLIB='ON',
        CLANG_LINK_CLANG_DYLIB='ON', CLANG_BUILD_TOOLS='OFF', CLANG_INCLUDE_TESTS='OFF')
    # bpftrace uses Clang's parser/codegen; it never invokes the separate static
    # analyzer or Objective-C source rewriter.
    flags.update(CLANG_ENABLE_STATIC_ANALYZER='OFF',CLANG_ENABLE_OBJC_REWRITER='OFF')
    return ['-D'+key+'='+str(value) for key,value in flags.items()]


def llvm(arch, jobs):
    src = source('llvm-project-23.1.4.src.tar.xz')
    output = BASE/arch/'llvm'
    env=environment(arch)
    run(['cmake','-G','Ninja','-S',src/'llvm','-B',output,*llvm_options(arch)], BASE/('llvm-'+arch+'-configure.log'),env=env)
    # Ninja does not track Git discovery environment changes in this header's
    # file dependencies, so refresh the upstream-generated identity explicitly.
    run(['cmake','-DNAMES=LLVM','-DLLVM_SOURCE_DIR='+str(src/'llvm'),
         '-DHEADER_FILE='+str(output/'include/llvm/Support/VCSRevision.h'),
         '-P',src/'llvm/cmake/modules/GenerateVersionFromVCS.cmake'],
        output/'configure-vcs.log',env=env)
    run(['cmake','--build',output,'-j'+str(jobs),'--target','LLVM','clang-cpp','libclang','clang-resource-headers'], BASE/('llvm-'+arch+'-build.log'),env=env)


def dependencies(arch, jobs):
    env=environment(arch)
    prefix=BASE/arch/'root/usr'
    prefix.mkdir(parents=True, exist_ok=True)
    zsrc=source('zlib-1.3.2.tar.gz')
    zout=BASE/arch/'zlib'; zout.mkdir(exist_ok=True)
    run([zsrc/'configure','--prefix='+str(prefix)],zout/'configure.log',cwd=zout,env=env)
    run(['make','-j'+str(jobs)],zout/'build.log',cwd=zout,env=env)
    run(['make','install'],zout/'install.log',cwd=zout,env=env)
    esrc=source('elfutils-0.194.tar.bz2')
    eout=BASE/arch/'elfutils'; eout.mkdir(exist_ok=True)
    run([esrc/'configure','--host='+TARGETS[arch][2],'--prefix='+str(prefix),
         '--disable-debuginfod','--disable-libdebuginfod','--disable-nls','--disable-werror',
         '--without-lzma','--without-bzlib','--without-zstd'],eout/'configure.log',cwd=eout,env=env)
    # libelf is the sole dependency of the original Linux libbpf and bpftool.
    run(['make','-j'+str(jobs),'-C','lib'],eout/'build-lib.log',cwd=eout,env=env)
    run(['make','-j'+str(jobs),'-C','libelf'],eout/'build-libelf.log',cwd=eout,env=env)
    run(['make','-C','libelf','install'],eout/'install-libelf.log',cwd=eout,env=env)


def linux_source():
    row=references()['linux']
    src=ROOT/'references/linux'
    actual=subprocess.check_output(['git','-C',src,'rev-parse','HEAD'],text=True).strip()
    if row[4]!=LINUX_REVISION or actual!=LINUX_REVISION:
        raise RuntimeError('fixed Linux source identity mismatch')
    subprocess.run(['git','-C',src,'diff','--quiet',LINUX_REVISION,'--'],check=True)
    return src


def bpftool(arch,jobs):
    src=linux_source(); env=environment(arch)
    prefix=BASE/arch/'root/usr'; out=BASE/arch/'bpftool'; out.mkdir(exist_ok=True)
    run(['make','-C',src/'tools/bpf/bpftool','-j'+str(jobs),'OUTPUT='+str(out)+'/',
         'ARCH='+arch,
         'CC='+env['CC'],'AR='+env['AR'],'CROSS_COMPILE='+TARGETS[arch][1],
         'LDFLAGS=-L'+str(prefix/'lib'),
         'EXTRA_CFLAGS=-I'+str(prefix/'include'),'SKIP_LLVM=1','SKIP_LIBBFD=1','SKIP_CRYPTO=1'],out/'build.log',env=env)
    (prefix/'bin').mkdir(exist_ok=True)
    shutil.copy2(out/'bpftool',prefix/'bin/bpftool')


def libbpf(arch,jobs):
    src=source('libbpf-85c0d575.tar.gz');env=environment(arch)
    prefix=BASE/arch/'root/usr';out=BASE/arch/'libbpf';out.mkdir(exist_ok=True)
    run(['make','-C',src/'src','-j'+str(jobs),'OBJDIR='+str(out),
         'PREFIX='+str(prefix),'LIBDIR='+str(prefix/'lib'),
         'install','install_uapi_headers'],out/'build.log',env=env)


def consumers(arch,jobs):
    env=environment(arch);prefix=BASE/arch/'root/usr';directory=BASE/arch
    target_include=Path('/usr/riscv64-linux-gnu/include' if arch=='riscv' else '/opt/loongarch64-tools/target/usr/include')
    (directory/'root/work').mkdir(parents=True,exist_ok=True)
    (prefix/'bin').mkdir(exist_ok=True)
    run(['/usr/bin/clang-23','-target','bpf','-std=gnu11','-O2','-g',
         '-I'+str(ROOT/'references/linux/tools/include/uapi'),
         '-I'+str(directory/'bpftool/libbpf/include'),'-I'+str(target_include),
         ROOT/'tests/workloads/bpf/udp.bpf.c','-c','-o',directory/'root/work/udp.bpf.o'],directory/'compile-bpf.log')
    run([env['CC'],'-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror',
         '-I'+str(directory/'bpftool/libbpf/include'),'-I'+str(prefix/'include'),
         ROOT/'tests/workloads/bpf/udp.c',directory/'bpftool/libbpf/libbpf.a',
         '-L'+str(prefix/'lib'),'-lelf','-lz','-o',prefix/'bin/bpf-udp'],directory/'compile-udp.log')
    run([env['CC'],'-std=gnu11','-O2','-Wall','-Wextra','-Werror',ROOT/'tests/workloads/bpf/syscalls.c',
         '-o',prefix/'bin/bpf-syscalls'],directory/'compile-syscalls.log')
    run([env['CC'],'-std=gnu11','-static','-O2','-Wall','-Wextra','-Werror',ROOT/'tests/workloads/bpf/init.c',
         '-o',directory/'root/init'],directory/'compile-init.log')


def bcc(arch,jobs):
    src=source('bcc-0.35.0.tar.gz');env=environment(arch)
    prefix=BASE/arch/'root/usr';out=BASE/arch/'bcc';llvm=BASE/arch/'llvm'
    machine,compiler,triple=TARGETS[arch]
    args=dict(CMAKE_BUILD_TYPE='Release',CMAKE_SYSTEM_NAME='Linux',CMAKE_SYSTEM_PROCESSOR=machine,
         CMAKE_C_COMPILER=compiler+'gcc',CMAKE_CXX_COMPILER=compiler+'g++',
         CMAKE_INSTALL_PREFIX=prefix,CMAKE_PREFIX_PATH=prefix,LLVM_DIR=llvm/'lib/cmake/llvm',
         CMAKE_FIND_ROOT_PATH=str(prefix)+';'+('/usr/riscv64-linux-gnu' if arch=='riscv' else '/opt/loongarch64-tools/target'),
         CMAKE_FIND_ROOT_PATH_MODE_LIBRARY='ONLY',CMAKE_FIND_ROOT_PATH_MODE_INCLUDE='ONLY',
         CMAKE_FIND_ROOT_PATH_MODE_PROGRAM='NEVER',CMAKE_DISABLE_FIND_PACKAGE_LibLzma='TRUE',
         CMAKE_DISABLE_FIND_PACKAGE_LibDebuginfod='TRUE',
         ENABLE_LLVM_SHARED='ON',ENABLE_LLVM_NATIVECODEGEN='OFF',ENABLE_RTTI='ON',
         ENABLE_CLANG_JIT='ON',ENABLE_USDT='ON',ENABLE_CPP_API='OFF',
         ENABLE_EXAMPLES='OFF',ENABLE_MAN='OFF',ENABLE_TESTS='OFF',
         ENABLE_LIBDEBUGINFOD='OFF',CMAKE_USE_LIBBPF_PACKAGE='ON',
         LIBBPF_INCLUDE_DIR=prefix/'include',LIBBPF_LIBRARIES=prefix/'lib/libbpf.so',
         LIBBPF_STATIC_LIBRARIES=prefix/'lib/libbpf.a',
         LIBELF_INCLUDE_DIRS=prefix/'include',LIBELF_LIBRARIES=prefix/'lib/libelf.so',
         CMAKE_POLICY_VERSION_MINIMUM='3.5',REVISION='0.35.0')
    # bpf-shared is upstream's independent runtime target, without the BCC compiler.
    # Configure the other targets against the actual in-progress cross LLVM tree.
    for component in ('Analysis','AST','Basic','CodeGen','Driver','Edit','Frontend','Lex',
                      'Parse','Rewrite','Sema','Serialization','ASTMatchers','Support','APINotes'):
        args['libclang'+component]=llvm/'lib/libclang-cpp.so'
    args['libclang-shared']=llvm/'lib/libclang-cpp.so'
    run(['cmake','-G','Ninja','-S',src,'-B',out,*['-D'+k+'='+str(v) for k,v in args.items()]],out/'configure.log',env=env)
    run(['cmake','--build',out,'-j'+str(jobs),'--target','bpf-shared'],out/'build.log',env=env)
    for path in (out/'src/cc').glob('libbcc_bpf.so*'):
        shutil.copy2(path,prefix/'lib'/path.name,follow_symlinks=True)
    headers=prefix/'include/bcc';headers.mkdir(exist_ok=True)
    for path in (src/'src/cc').glob('*.h'):shutil.copy2(path,headers/path.name)


def bpftrace(arch,jobs,objects=False):
    src=source('bpftrace-0.27.1.tar.gz');cereal=source('cereal-1.3.2.tar.gz')
    prefix=BASE/arch/'root/usr';out=BASE/arch/'bpftrace';llvm=BASE/arch/'llvm'
    env=environment(arch);machine,compiler,triple=TARGETS[arch]
    args=dict(CMAKE_BUILD_TYPE='Release',CMAKE_SYSTEM_NAME='Linux',CMAKE_SYSTEM_PROCESSOR=machine,
        CMAKE_C_COMPILER=compiler+'gcc',CMAKE_CXX_COMPILER=compiler+'g++',
        CMAKE_INSTALL_PREFIX=prefix,CMAKE_PREFIX_PATH=prefix,
        CMAKE_EXE_LINKER_FLAGS='-L'+str(prefix/'lib')+' -Wl,-rpath-link,'+str(prefix/'lib')+' -Wl,-rpath-link,'+str(llvm/'lib'),
        CMAKE_FIND_ROOT_PATH=str(prefix)+';'+('/usr/riscv64-linux-gnu' if arch=='riscv' else '/opt/loongarch64-tools/target'),
        CMAKE_FIND_ROOT_PATH_MODE_LIBRARY='ONLY',CMAKE_FIND_ROOT_PATH_MODE_INCLUDE='ONLY',
        CMAKE_FIND_ROOT_PATH_MODE_PROGRAM='NEVER',
        LLVM_DIR=llvm/'lib/cmake/llvm',Clang_DIR=llvm/'lib/cmake/clang',
        ENABLE_MAN='OFF',BUILD_TESTING='OFF',ENABLE_SKB_OUTPUT='OFF',ENABLE_SYSTEMD='OFF',
        USE_SYSTEM_LIBBPF='ON',SYSTEM_INCLUDE_PATHS='/usr/include',
        LIBBPF_INCLUDE_DIRS=prefix/'include',LIBBPF_LIBRARIES=prefix/'lib/libbpf.a',
        LIBBCC_INCLUDE_DIRS=prefix/'include',LIBBCC_LIBRARIES=prefix/'lib/libbcc_bpf.so',
        LIBBCC_BPF_LIBRARIES=prefix/'lib/libbcc_bpf.so',
        LIBELF_INCLUDE_DIRS=prefix/'include',LIBELF_LIBRARIES=prefix/'lib/libelf.so',
        LIBCEREAL_INCLUDE_DIRS=cereal/'include',ZLIB_INCLUDE_DIR=prefix/'include',
        ZLIB_LIBRARY=prefix/'lib/libz.so')
    run(['cmake','-G','Ninja','-S',src,'-B',out,*['-D'+k+'='+str(v) for k,v in args.items()]],out/'configure.log',env=env)
    run(['cmake','--build',out,'--target','version_h'],out/'version.log',env=env)
    if objects:
        targets=subprocess.check_output(['ninja','-C',out,'-t','targets','all'],text=True)
        targets=[line.split(': ',1)[0] for line in targets.splitlines() if ': CXX_COMPILER__' in line]
        run(['ninja','-C',out,'-j'+str(jobs),*targets],out/'compile.log',env=env)
        return
    run(['cmake','--build',out,'-j'+str(jobs),'--target','bpftrace'],out/'build.log',env=env)
    shutil.copy2(out/'src/bpftrace',prefix/'bin/bpftrace')


def bpftrace_objects(arch,jobs):
    bpftrace(arch,jobs,objects=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch',choices=TARGETS,required=True)
    parser.add_argument('--stage',choices=('all','dependencies','bpftool','libbpf','consumers','bcc','llvm','bpftrace','bpftrace_objects'),required=True)
    parser.add_argument('--jobs',type=int,default=2)
    args=parser.parse_args()
    if args.jobs<1: parser.error('--jobs must be positive')
    stages=('llvm','dependencies','bpftool','libbpf','consumers','bcc','bpftrace') if args.stage=='all' else (args.stage,)
    for stage in stages:globals()[stage](args.arch,args.jobs)

if __name__=='__main__': main()
