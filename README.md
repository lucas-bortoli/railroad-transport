<center>
  <img src="docs/logo.png">
</center>

# Compilar e executar no Windows

É preciso usar o MSYS2, no ambiente MSYS (camada de compatibilidade POSIX) exclusivamente.

Instale o toolchain:

```sh
pacman -S base-devel gcc
```

Após compilar, para executar o binário resultante em computadores sem o MSYS2, é preciso distribuir a DLL `C:\msys64\usr\bin\msys-2.0.dll` ao lado da aplicação.
