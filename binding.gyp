{
  'includes': [ 'deps/common-sqlite.gypi' ],
  'variables': {
      'sqlite%':'internal',
  },
  'targets': [
    {
      'target_name': 'node_sqlite3',
      'conditions': [
        ['sqlite != "internal"', {
            'libraries': [
               '-L<@(sqlite)/lib',
               '-lsqlite3'
            ],
            'include_dirs': [ '<@(sqlite)/include' ],
            'conditions': [ [ 'OS=="linux"', {'libraries+':['-Wl,-rpath=<@(sqlite)/lib']} ] ]
        },
        {
            'dependencies': [
              'deps/sqlite3.gyp:sqlite3'
            ]
        }
        ]
      ],
      'include_dirs': [
          '<(node_root_dir)/deps'
      ],
      'libraries': [
          '-lz'
      ],
      'sources': [
        'src/database.cc',
        'src/node_sqlite3.cc',
        'src/statement.cc',
        'src/ioapi.c',
        'src/unzip.c',
        'src/minizip.c'
      ],
    }
  ]
}
