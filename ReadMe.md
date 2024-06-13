If it's **the first time** you check-out a repo you need to use --init first:  
```
git submodule update --init --recursive  
```  
For **git 1.8.2** or above, the option ```--remote``` was added to support updating to latest tips of remote branches:
```
git submodule update --recursive --remote
```
## Update specific submodules
```
git submodule update --init path/to/submodule
```
Replace path/to/submodule with the path to the specific submodule you want to update or initialize.

If you have nested submodules, you may need to use the --recursive flag to update all the submodules recursively. For example:
```
git submodule update --init --recursive path/to/submodule
```
These commands ensure that only the specified submodule is updated or initialized, without affecting other submodules in your Git repository.  
# Configure 

```
git submodule update --init --recursive
make toolchain-cmake
```

# How to build && install 
*Build & install llvm tool-chain*  
- make toolchain-cmake
- make toolchain-llvm-main
- make  toolchain-protoc 
- make toolchain-onnx-mlir
 
## make toolchain-onnx-mlir fails  
`cd toolchain/onnx-mlir/build`  
- `make -j8`  
- `make install -j8`  

## make toolchain-llvm-main fails  
`cd toolchain/riscv-llvm/build`
- `make -j8` 
- `make install -j8` 

If only changes were made to llvm and running the command `make -j8` fails, run the initial command `make toolchain-llvm-main`

# Python environment

```
wget https://repo.anaconda.com/miniconda/Miniconda3-py310_24.3.0-0-Linux-x86_64.sh
bash Miniconda3-py310_24.3.0-0-Linux-x86_64.sh 
```
create a Conda environment  
```
source ~/miniconda3/etc/profile.d/conda.sh
conda create --name onnx-mlir --file packages.txt
```
