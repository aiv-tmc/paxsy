{ stdenv, lib }:
stdenv.mkDerivation rec {
  pname = "paxsy";
  version = "1.0.0";
  src = lib.cleanSourceWith {
    src = ../.;
    filter = path: type:
      let name = builtins.baseNameOf path;
      in name != ".git"
         && name != "bin"
         && name != "build"
         && name != "result"
         && name != "node_modules"
         && name != "paxsy"
         && !(lib.hasSuffix ".o" name)
         && !(lib.hasSuffix ".pyc" name);
  };

  buildPhase = ''
    runHook preBuild
    make
    runHook postBuild
  '';
  doCheck = true;
  checkPhase = ''
    runHook preCheck
    make test
    runHook postCheck
  '';
  installPhase = ''
    runHook preInstall
    install -Dm755 paxsy $out/bin/paxsy
    install -d $out/lib/paxsy
    cp -r lib/std $out/lib/paxsy/std
    chmod -R u+w $out/lib/paxsy/std
    install -Dm644 README.md $out/share/doc/paxsy/README.md
    install -Dm644 docs/LanguageSpecification.md $out/share/doc/paxsy/LanguageSpecification.md
    install -Dm644 docs/ToolchainUserManual.md $out/share/doc/paxsy/ToolchainUserManual.md
    install -Dm644 docs/StandardLibraryReference.md $out/share/doc/paxsy/StandardLibraryReference.md
    install -Dm644 docs/LanguageDeveloperGuide.md $out/share/doc/paxsy/LanguageDeveloperGuide.md
    install -Dm644 docs/LLMIntegrationGuide.md $out/share/doc/paxsy/LLMIntegrationGuide.md
    install -Dm644 LICENSE $out/share/licenses/paxsy/LICENSE
    install -Dm644 NOTICE $out/share/doc/paxsy/NOTICE
    install -Dm644 editors/vim/ftdetect/px.vim $out/share/vim/vimfiles/ftdetect/px.vim
    install -Dm644 editors/vim/syntax/px.vim $out/share/vim/vimfiles/syntax/px.vim
    install -d $out/share/vscode/extensions/paxsy-language
    cp -r editors/vscode/. $out/share/vscode/extensions/paxsy-language/
    runHook postInstall
  '';
  meta = {
    description = "Compiler and toolchain for the Paxsy systems programming language";
    homepage = "https://github.com/aiv-tmc/paxsy";
    license = lib.licenses.asl20;
    platforms = [ "x86_64-linux" ];
    mainProgram = "paxsy";
  };
}
