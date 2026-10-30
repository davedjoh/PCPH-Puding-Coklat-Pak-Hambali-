{
  description = "dev shell";

  inputs.nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      # Gunakan python312, bukan python311
      py = pkgs.python312;
    in {
      devShells.${system}.default = pkgs.mkShell {
        packages = [
          py
          py.pkgs.httpx
          py.pkgs.websockets
          py.pkgs.python-lsp-server
          py.pkgs.python-lsp-ruff  # Fast linting and formatting
          py.pkgs.pylsp-mypy       # Type checking
          py.pkgs.pylsp-rope       # Refactoring support
          pkgs.cloudflared
          pkgs.websocat
        ];
      };
    };
}
