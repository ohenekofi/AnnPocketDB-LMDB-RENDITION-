{
  "targets": [
    {
      "target_name": "annpocketdb_native",
      "sources": [ "src/addon.cpp" ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "../../include"
      ],
      "libraries": [ "-lannpocketdb_c" ],
      "defines": [ "NAPI_DISABLE_CPP_EXCEPTIONS" ],
      "cflags_cc": [ "-std=c++17", "-fexceptions" ],
      "conditions": [
        ["OS=='win'", {
          "libraries": [ "annpocketdb_c.lib" ],
          "msvs_settings": { "VCCLCompilerTool": { "ExceptionHandling": 1 } }
        }],
        ["OS=='mac'", {
          "xcode_settings": {
            "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
            "MACOSX_DEPLOYMENT_TARGET": "10.15"
          }
        }]
      ]
    }
  ]
}
