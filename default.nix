{ pkgs ? import <nixpkgs> {} }:
{
  paxsy = pkgs.callPackage ./nix/package.nix {};
}
