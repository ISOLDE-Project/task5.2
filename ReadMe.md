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