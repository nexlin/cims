import sys
import os
import glob
import importlib, importlib.util
from typing import Optional


class ModuleLoader:

    @staticmethod
    def loadModule(module_dir: str, module_prefix: str, name: str, *args, **kwargs) -> Optional[object]:
        for module_file in glob.glob(f'{module_dir}/**/{module_prefix}*.py', recursive=True):
            module_dir, module_name = os.path.split(module_file)
            module_name = module_name.replace(".py", "")
            object_name = module_name[len(module_prefix):]
            if object_name.lower() == name.lower():
                spec = importlib.util.spec_from_file_location(module_name, module_file)
                module = importlib.util.module_from_spec(spec)
                sys.modules[spec.name] = module
                spec.loader.exec_module(module)
                obj = getattr(module, object_name)
                return obj(*args, **kwargs)
        return None
