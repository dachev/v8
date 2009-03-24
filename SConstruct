# Copyright 2008 the V8 project authors. All rights reserved.
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
#       copyright notice, this list of conditions and the following
#       disclaimer in the documentation and/or other materials provided
#       with the distribution.
#     * Neither the name of Google Inc. nor the names of its
#       contributors may be used to endorse or promote products derived
#       from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

import platform
import re
import sys
import os
from os.path import join, dirname, abspath
from types import DictType, StringTypes
root_dir = dirname(File('SConstruct').rfile().abspath)
sys.path.append(join(root_dir, 'tools'))
import js2c, utils

# ANDROID_TOP is the top of the Android checkout, fetched from the environment
# variable 'TOP'.   You will also need to set the CXX, CC, AR and RANLIB
# environment variables to the cross-compiling tools.
ANDROID_TOP = os.environ.get('TOP')
if ANDROID_TOP is None:
  ANDROID_TOP=""

ANDROID_FLAGS = ['-march=armv5te',
                 '-mtune=xscale',
                 '-msoft-float',
                 '-fpic',
                 '-mthumb-interwork',
                 '-funwind-tables',
                 '-fstack-protector',
                 '-fno-short-enums',
                 '-fmessage-length=0',
                 '-finline-functions',
                 '-fno-inline-functions-called-once',
                 '-fgcse-after-reload',
                 '-frerun-cse-after-loop',
                 '-frename-registers',
                 '-fomit-frame-pointer',
                 '-fno-strict-aliasing',
                 '-finline-limit=64',
                 '-MD']

ANDROID_INCLUDES = [ANDROID_TOP + '/bionic/libc/arch-arm/include',
                    ANDROID_TOP + '/bionic/libc/include',
                    ANDROID_TOP + '/bionic/libstdc++/include',
                    ANDROID_TOP + '/bionic/libc/kernel/common',
                    ANDROID_TOP + '/bionic/libc/kernel/arch-arm',
                    ANDROID_TOP + '/bionic/libm/include',
                    ANDROID_TOP + '/bionic/libm/include/arch/arm',
                    ANDROID_TOP + '/bionic/libthread_db/include']

LIBRARY_FLAGS = {
  'all': {
    'CPPDEFINES':   ['ENABLE_LOGGING_AND_PROFILING']
  },
  'gcc': {
    'all': {
      'CCFLAGS':      ['$DIALECTFLAGS', '$WARNINGFLAGS'],
      'CXXFLAGS':     ['$CCFLAGS', '-fno-rtti', '-fno-exceptions'],
    },
    'mode:debug': {
      'CCFLAGS':      ['-g', '-O0'],
      'CPPDEFINES':   ['ENABLE_DISASSEMBLER', 'DEBUG']
    },
    'mode:release': {
      'CCFLAGS':      ['-O3', '-fomit-frame-pointer', '-fdata-sections',
                       '-ffunction-sections'],
      'os:android': {
        'CPPDEFINES': ['SK_RELEASE', 'NDEBUG']
      }
    },
    'os:linux': {
      'CCFLAGS':      ['-ansi'],
    },
    'os:macos': {
      'CCFLAGS':      ['-ansi'],
    },
    'os:freebsd': {
      'CCFLAGS':      ['-ansi'],
    },
    'os:win32': {
      'CCFLAGS':      ['-DWIN32'],
      'CXXFLAGS':     ['-DWIN32'],
    },
    'os:android': {
      'CPPDEFINES':   ['ANDROID', '__ARM_ARCH_5__', '__ARM_ARCH_5T__',
                       '__ARM_ARCH_5E__', '__ARM_ARCH_5TE__'],
      'CCFLAGS':      ANDROID_FLAGS,
      'WARNINGFLAGS': ['-Wall', '-Wno-unused', '-Werror=return-type',
                       '-Wstrict-aliasing=2'],
      'CPPPATH':      ANDROID_INCLUDES,
    },
    'wordsize:64': {
      'CCFLAGS':      ['-m32'],
      'LINKFLAGS':    ['-m32']
    },
    'prof:oprofile': {
      'CPPDEFINES':   ['ENABLE_OPROFILE_AGENT']
    }
  },
  'msvc': {
    'all': {
      'DIALECTFLAGS': ['/nologo'],
      'CCFLAGS':      ['$DIALECTFLAGS', '$WARNINGFLAGS'],
      'CXXFLAGS':     ['$CCFLAGS', '/GR-', '/Gy'],
      'CPPDEFINES':   ['WIN32', '_USE_32BIT_TIME_T', 'PCRE_STATIC'],
      'LINKFLAGS':    ['/NOLOGO', '/MACHINE:X86', '/INCREMENTAL:NO',
          '/NXCOMPAT', '/IGNORE:4221'],
      'ARFLAGS':      ['/NOLOGO'],
      'CCPDBFLAGS':   ['/Zi']
    },
    'mode:debug': {
      'CCFLAGS':      ['/Od', '/Gm'],
      'CPPDEFINES':   ['_DEBUG', 'ENABLE_DISASSEMBLER', 'DEBUG'],
      'LINKFLAGS':    ['/DEBUG'],
      'msvcrt:static': {
        'CCFLAGS': ['/MTd']
      },
      'msvcrt:shared': {
        'CCFLAGS': ['/MDd']
      }
    },
    'mode:release': {
      'CCFLAGS':      ['/O2', '/GL'],
      'LINKFLAGS':    ['/OPT:REF', '/OPT:ICF', '/LTCG'],
      'ARFLAGS':      ['/LTCG'],
      'msvcrt:static': {
        'CCFLAGS': ['/MT']
      },
      'msvcrt:shared': {
        'CCFLAGS': ['/MD']
      }
    },
  }
}


V8_EXTRA_FLAGS = {
  'gcc': {
    'all': {
      'CXXFLAGS':     [], #['-fvisibility=hidden'],
      'WARNINGFLAGS': ['-Wall', '-Werror', '-W',
          '-Wno-unused-parameter']
    },
    'arch:arm': {
      'CPPDEFINES':   ['ARM']
    },
    'arch:android': {
      'CPPDEFINES':   ['ARM']
    },
    'os:win32': {
      'WARNINGFLAGS': ['-pedantic', '-Wno-long-long']
    },
    'os:linux': {
      'WARNINGFLAGS': ['-pedantic']
    },
    'os:macos': {
      'WARNINGFLAGS': ['-pedantic']
    },
    'disassembler:on': {
      'CPPDEFINES':   ['ENABLE_DISASSEMBLER']
    }
  },
  'msvc': {
    'all': {
      'WARNINGFLAGS': ['/W3', '/WX', '/wd4355', '/wd4800']
    },
    'library:shared': {
      'CPPDEFINES':   ['BUILDING_V8_SHARED'],
      'LIBS': ['winmm', 'ws2_32']
    },
    'arch:arm': {
      'CPPDEFINES':   ['ARM'],
      # /wd4996 is to silence the warning about sscanf
      # used by the arm simulator.
      'WARNINGFLAGS': ['/wd4996']
    },
    'disassembler:on': {
      'CPPDEFINES':   ['ENABLE_DISASSEMBLER']
    }
  }
}


MKSNAPSHOT_EXTRA_FLAGS = {
  'gcc': {
    'os:linux': {
      'LIBS': ['pthread', 'rt'],
    },
    'os:macos': {
      'LIBS': ['pthread'],
    },
    'os:freebsd': {
      'LIBS': ['pthread'],
    },
    'os:win32': {
      'LIBS': ['winmm', 'ws2_32'],
    },
  },
  'msvc': {
    'all': {
      'LIBS': ['winmm', 'ws2_32']
    }
  }
}


DTOA_EXTRA_FLAGS = {
  'gcc': {
    'all': {
      'WARNINGFLAGS': ['-Werror', '-Wno-uninitialized']
    }
  },
  'msvc': {
    'all': {
      'WARNINGFLAGS': ['/WX', '/wd4018', '/wd4244']
    }
  }
}


CCTEST_EXTRA_FLAGS = {
  'all': {
    'CPPPATH': [join(root_dir, 'src')],
    'LIBS': ['$LIBRARY']
  },
  'gcc': {
    'all': {
      'LIBPATH': [abspath('.')]
    },
    'os:linux': {
      'LIBS':         ['pthread', 'rt'],
    },
    'os:macos': {
      'LIBS':         ['pthread'],
    },
    'os:freebsd': {
      'LIBS':         ['execinfo', 'pthread']
    },
    'os:win32': {
      'LIBS': ['winmm', 'ws2_32']
    },
    'wordsize:64': {
      'CCFLAGS':      ['-m32'],
      'LINKFLAGS':    ['-m32']
    },
  },
  'msvc': {
    'all': {
      'CPPDEFINES': ['_HAS_EXCEPTIONS=0'],
      'LIBS': ['winmm', 'ws2_32']
    },
    'library:shared': {
      'CPPDEFINES': ['USING_V8_SHARED']
    }
  }
}


SAMPLE_FLAGS = {
  'all': {
    'CPPPATH': [join(abspath('.'), 'include')],
    'LIBS': ['$LIBRARY'],
  },
  'gcc': {
    'all': {
      'LIBPATH': ['.'],
      'CCFLAGS': ['-fno-rtti', '-fno-exceptions']
    },
    'os:linux': {
      'LIBS':         ['pthread', 'rt'],
    },
    'os:macos': {
      'LIBS':         ['pthread'],
    },
    'os:freebsd': {
      'LIBS':         ['execinfo', 'pthread']
    },
    'os:win32': {
      'LIBS':         ['winmm', 'ws2_32']
    },
    'os:android': {
      'CPPDEFINES':   ['ANDROID', '__ARM_ARCH_5__', '__ARM_ARCH_5T__',
                       '__ARM_ARCH_5E__', '__ARM_ARCH_5TE__'],
      'CCFLAGS':      ANDROID_FLAGS,
      'CPPPATH':      ANDROID_INCLUDES,
      'LIBPATH':     [ANDROID_TOP + '/out/target/product/generic/obj/lib'],
      'LINKFLAGS':    ['-nostdlib',
                       '-Bdynamic',
                       '-Wl,-T,' + ANDROID_TOP + '/build/core/armelf.x',
                       '-Wl,-dynamic-linker,/system/bin/linker',
                       '-Wl,--gc-sections',
                       '-Wl,-z,nocopyreloc',
                       '-Wl,-rpath-link=' + ANDROID_TOP + '/out/target/product/generic/obj/lib',
                       ANDROID_TOP + '/out/target/product/generic/obj/lib/crtbegin_dynamic.o',
                       ANDROID_TOP + '/prebuilt/linux-x86/toolchain/arm-eabi-4.2.1/lib/gcc/arm-eabi/4.2.1/interwork/libgcc.a',
                       ANDROID_TOP + '/out/target/product/generic/obj/lib/crtend_android.o'],
      'LIBS':         ['c', 'stdc++', 'm'],
      'mode:release': {
        'CPPDEFINES': ['SK_RELEASE', 'NDEBUG']
      }
    },
    'wordsize:64': {
      'CCFLAGS':      ['-m32'],
      'LINKFLAGS':    ['-m32']
    },
    'mode:release': {
      'CCFLAGS':      ['-O2']
    },
    'mode:debug': {
      'CCFLAGS':      ['-g', '-O0']
    },
    'prof:oprofile': {
      'LIBPATH': ['/usr/lib32', '/usr/lib32/oprofile'],
      'LIBS': ['opagent']
    }
  },
  'msvc': {
    'all': {
      'CCFLAGS': ['/nologo'],
      'LINKFLAGS': ['/nologo'],
      'LIBS': ['winmm', 'ws2_32']
    },
    'library:shared': {
      'CPPDEFINES': ['USING_V8_SHARED']
    },
    'prof:on': {
      'LINKFLAGS': ['/MAP']
    },
    'mode:release': {
      'CCFLAGS':   ['/O2'],
      'LINKFLAGS': ['/OPT:REF', '/OPT:ICF', '/LTCG'],
      'msvcrt:static': {
        'CCFLAGS': ['/MT']
      },
      'msvcrt:shared': {
        'CCFLAGS': ['/MD']
      }
    },
    'mode:debug': {
      'CCFLAGS':   ['/Od'],
      'LINKFLAGS': ['/DEBUG'],
      'msvcrt:static': {
        'CCFLAGS': ['/MTd']
      },
      'msvcrt:shared': {
        'CCFLAGS': ['/MDd']
      }
    }
  }
}


D8_FLAGS = {
  'gcc': {
    'console:readline': {
      'LIBS': ['readline']
    },
    'os:linux': {
      'LIBS': ['pthread', 'rt'],
    },
    'os:macos': {
      'LIBS': ['pthread'],
    },
    'os:freebsd': {
      'LIBS': ['pthread'],
    },
    'os:win32': {
      'LIBS': ['winmm', 'ws2_32'],
    },
  },
  'msvc': {
    'all': {
      'LIBS': ['winmm', 'ws2_32']
    }
  }
}


SUFFIXES = {
  'release': '',
  'debug': '_g'
}


def Abort(message):
  print message
  sys.exit(1)


def GuessToolchain(os):
  tools = Environment()['TOOLS']
  if 'gcc' in tools:
    return 'gcc'
  elif 'msvc' in tools:
    return 'msvc'
  else:
    return None


OS_GUESS = utils.GuessOS()
TOOLCHAIN_GUESS = GuessToolchain(OS_GUESS)
ARCH_GUESS = utils.GuessArchitecture()
WORDSIZE_GUESS = utils.GuessWordsize()


SIMPLE_OPTIONS = {
  'toolchain': {
    'values': ['gcc', 'msvc'],
    'default': TOOLCHAIN_GUESS,
    'help': 'the toolchain to use'
  },
  'os': {
    'values': ['freebsd', 'linux', 'macos', 'win32', 'android'],
    'default': OS_GUESS,
    'help': 'the os to build for'
  },
  'arch': {
    'values':['arm', 'ia32'],
    'default': ARCH_GUESS,
    'help': 'the architecture to build for'
  },
  'snapshot': {
    'values': ['on', 'off', 'nobuild'],
    'default': 'off',
    'help': 'build using snapshots for faster start-up'
  },
  'prof': {
    'values': ['on', 'off', 'oprofile'],
    'default': 'off',
    'help': 'enable profiling of build target'
  },
  'library': {
    'values': ['static', 'shared'],
    'default': 'static',
    'help': 'the type of library to produce'
  },
  'msvcrt': {
    'values': ['static', 'shared'],
    'default': 'static',
    'help': 'the type of MSVCRT library to use'
  },
  'wordsize': {
    'values': ['64', '32'],
    'default': WORDSIZE_GUESS,
    'help': 'the word size'
  },
  'simulator': {
    'values': ['arm', 'none'],
    'default': 'none',
    'help': 'build with simulator'
  },
  'disassembler': {
    'values': ['on', 'off'],
    'default': 'off',
    'help': 'enable the disassembler to inspect generated code'
  },
  'sourcesignatures': {
    'values': ['MD5', 'timestamp'],
    'default': 'MD5',
    'help': 'set how the build system detects file changes'
  },
  'console': {
    'values': ['dumb', 'readline'],
    'default': 'dumb',
    'help': 'the console to use for the d8 shell'
  }
}


def GetOptions():
  result = Options()
  result.Add('mode', 'compilation mode (debug, release)', 'release')
  result.Add('sample', 'build sample (shell, process)', '')
  result.Add('env', 'override environment settings (NAME1:value1,NAME2:value2)', '')
  for (name, option) in SIMPLE_OPTIONS.iteritems():
    help = '%s (%s)' % (name, ", ".join(option['values']))
    result.Add(name, help, option.get('default'))
  return result


def SplitList(str):
  return [ s for s in str.split(",") if len(s) > 0 ]


def IsLegal(env, option, values):
  str = env[option]
  for s in SplitList(str):
    if not s in values:
      Abort("Illegal value for option %s '%s'." % (option, s))
      return False
  return True


def VerifyOptions(env):
  if not IsLegal(env, 'mode', ['debug', 'release']):
    return False
  if not IsLegal(env, 'sample', ["shell", "process"]):
    return False
  if env['os'] == 'win32' and env['library'] == 'shared' and env['prof'] == 'on':
    Abort("Profiling on windows only supported for static library.")
  if env['prof'] == 'oprofile' and env['os'] != 'linux':
    Abort("OProfile is only supported on Linux.")
  for (name, option) in SIMPLE_OPTIONS.iteritems():
    if (not option.get('default')) and (name not in ARGUMENTS):
      message = ("A value for option %s must be specified (%s)." %
          (name, ", ".join(option['values'])))
      Abort(message)
    if not env[name] in option['values']:
      message = ("Unknown %s value '%s'.  Possible values are (%s)." %
          (name, env[name], ", ".join(option['values'])))
      Abort(message)


class BuildContext(object):

  def __init__(self, options, env_overrides, samples):
    self.library_targets = []
    self.mksnapshot_targets = []
    self.cctest_targets = []
    self.sample_targets = []
    self.d8_targets = []
    self.options = options
    self.env_overrides = env_overrides
    self.samples = samples
    self.use_snapshot = (options['snapshot'] != 'off')
    self.build_snapshot = (options['snapshot'] == 'on')
    self.flags = None

  def AddRelevantFlags(self, initial, flags):
    result = initial.copy()
    self.AppendFlags(result, flags.get('all'))
    toolchain = self.options['toolchain']
    if toolchain in flags:
      self.AppendFlags(result, flags[toolchain].get('all'))
      for option in sorted(self.options.keys()):
        value = self.options[option]
        self.AppendFlags(result, flags[toolchain].get(option + ':' + value))
    return result

  def AddRelevantSubFlags(self, options, flags):
    self.AppendFlags(options, flags.get('all'))
    for option in sorted(self.options.keys()):
      value = self.options[option]
      self.AppendFlags(options, flags.get(option + ':' + value))

  def GetRelevantSources(self, source):
    result = []
    result += source.get('all', [])
    for (name, value) in self.options.iteritems():
      result += source.get(name + ':' + value, [])
    return sorted(result)

  def AppendFlags(self, options, added):
    if not added:
      return
    for (key, value) in added.iteritems():
      if key.find(':') != -1:
        self.AddRelevantSubFlags(options, { key: value })
      else:
        if not key in options:
          options[key] = value
        else:
          prefix = options[key]
          if isinstance(prefix, StringTypes): prefix = prefix.split()
          options[key] = prefix + value

  def ConfigureObject(self, env, input, **kw):
    if (kw.has_key('CPPPATH') and env.has_key('CPPPATH')):
      kw['CPPPATH'] += env['CPPPATH']
    if self.options['library'] == 'static':
      return env.StaticObject(input, **kw)
    else:
      return env.SharedObject(input, **kw)

  def ApplyEnvOverrides(self, env):
    if not self.env_overrides:
      return
    if type(env['ENV']) == DictType:
      env['ENV'].update(**self.env_overrides)
    else:
      env['ENV'] = self.env_overrides


def PostprocessOptions(options):
  # Adjust architecture if the simulator option has been set
  if (options['simulator'] != 'none') and (options['arch'] != options['simulator']):
    if 'arch' in ARGUMENTS:
      # Print a warning if arch has explicitly been set
      print "Warning: forcing architecture to match simulator (%s)" % options['simulator']
    options['arch'] = options['simulator']


def ParseEnvOverrides(arg):
  # The environment overrides are in the format NAME1:value1,NAME2:value2
  overrides = {}
  for override in arg.split(','):
    pos = override.find(':')
    if pos == -1:
      continue
    overrides[override[:pos].strip()] = override[pos+1:].strip()
  return overrides


def BuildSpecific(env, mode, env_overrides):
  options = {'mode': mode}
  for option in SIMPLE_OPTIONS:
    options[option] = env[option]
  PostprocessOptions(options)

  context = BuildContext(options, env_overrides, samples=SplitList(env['sample']))

  library_flags = context.AddRelevantFlags(os.environ, LIBRARY_FLAGS)
  v8_flags = context.AddRelevantFlags(library_flags, V8_EXTRA_FLAGS)
  mksnapshot_flags = context.AddRelevantFlags(library_flags, MKSNAPSHOT_EXTRA_FLAGS)
  dtoa_flags = context.AddRelevantFlags(library_flags, DTOA_EXTRA_FLAGS)
  cctest_flags = context.AddRelevantFlags(v8_flags, CCTEST_EXTRA_FLAGS)
  sample_flags = context.AddRelevantFlags(os.environ, SAMPLE_FLAGS)
  d8_flags = context.AddRelevantFlags(library_flags, D8_FLAGS)

  context.flags = {
    'v8': v8_flags,
    'mksnapshot': mksnapshot_flags,
    'dtoa': dtoa_flags,
    'cctest': cctest_flags,
    'sample': sample_flags,
    'd8': d8_flags
  }

  target_id = mode
  suffix = SUFFIXES[target_id]
  library_name = 'v8' + suffix
  env['LIBRARY'] = library_name

  # Build the object files by invoking SCons recursively.
  (object_files, shell_files, mksnapshot) = env.SConscript(
    join('src', 'SConscript'),
    build_dir=join('obj', target_id),
    exports='context',
    duplicate=False
  )

  context.mksnapshot_targets.append(mksnapshot)

  # Link the object files into a library.
  env.Replace(**context.flags['v8'])
  context.ApplyEnvOverrides(env)
  if context.options['library'] == 'static':
    library = env.StaticLibrary(library_name, object_files)
  else:
    # There seems to be a glitch in the way scons decides where to put
    # PDB files when compiling using MSVC so we specify it manually.
    # This should not affect any other platforms.
    pdb_name = library_name + '.dll.pdb'
    library = env.SharedLibrary(library_name, object_files, PDB=pdb_name)
  context.library_targets.append(library)

  d8_env = Environment()
  d8_env.Replace(**context.flags['d8'])
  shell = d8_env.Program('d8' + suffix, object_files + shell_files)
  context.d8_targets.append(shell)

  for sample in context.samples:
    sample_env = Environment(LIBRARY=library_name)
    sample_env.Replace(**context.flags['sample'])
    context.ApplyEnvOverrides(sample_env)
    sample_object = sample_env.SConscript(
      join('samples', 'SConscript'),
      build_dir=join('obj', 'sample', sample, target_id),
      exports='sample context',
      duplicate=False
    )
    sample_name = sample + suffix
    sample_program = sample_env.Program(sample_name, sample_object)
    sample_env.Depends(sample_program, library)
    context.sample_targets.append(sample_program)

  cctest_program = env.SConscript(
    join('test', 'cctest', 'SConscript'),
    build_dir=join('obj', 'test', target_id),
    exports='context object_files',
    duplicate=False
  )
  context.cctest_targets.append(cctest_program)

  return context


def Build():
  opts = GetOptions()
  env = Environment(options=opts)
  Help(opts.GenerateHelpText(env))
  VerifyOptions(env)
  env_overrides = ParseEnvOverrides(env['env'])

  SourceSignatures(env['sourcesignatures'])

  libraries = []
  mksnapshots = []
  cctests = []
  samples = []
  d8s = []
  modes = SplitList(env['mode'])
  for mode in modes:
    context = BuildSpecific(env.Copy(), mode, env_overrides)
    libraries += context.library_targets
    mksnapshots += context.mksnapshot_targets
    cctests += context.cctest_targets
    samples += context.sample_targets
    d8s += context.d8_targets

  env.Alias('library', libraries)
  env.Alias('mksnapshot', mksnapshots)
  env.Alias('cctests', cctests)
  env.Alias('sample', samples)
  env.Alias('d8', d8s)

  if env['sample']:
    env.Default('sample')
  else:
    env.Default('library')


# We disable deprecation warnings because we need to be able to use
# env.Copy without getting warnings for compatibility with older
# version of scons.  Also, there's a bug in some revisions that
# doesn't allow this flag to be set, so we swallow any exceptions.
# Lovely.
try:
  SetOption('warn', 'no-deprecated')
except:
  pass


Build()
