if git_prog.found()
  version_vcs_command = [git_prog, 'log', '-1', '--format=%x22-git-%h%x22']
else
  version_vcs_command = [sh, '-c', 'exit 1']
endif

version_vcs_source = vcs_tag(
  input: 'version-vcs.cpp.in',
  output: 'version-vcs.cpp',
  fallback: '""',
  command: version_vcs_command,
)
