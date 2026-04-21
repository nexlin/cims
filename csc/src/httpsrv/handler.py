import pandas as pd
from typing import List, Union, Dict, Callable, Awaitable, Optional, Any
from dataclasses import dataclass, field


CSVRow = Dict[str, str]
CSVLike = List[CSVRow]
BodyData = Union[None, pd.DataFrame, CSVLike, Dict, bytes, str, Any]


@dataclass
class HandlerResult:
    status: int = 200
    body: Optional[BodyData] = None
    headers: Dict[str, str] = field(default_factory=dict)
    media_type: Optional[str] = None  #"application/json", "text/csv"
    response: Any = None  # FastAPI Response object


@dataclass
class HandlerArgs:
    method: str
    full_path: str
    client_ip: str
    client_port: int
    query_params: Dict[str, str] = field(default_factory=dict)
    headers: Dict[str, str] = field(default_factory=dict)
    cookies: Dict[str, str] = field(default_factory=dict)
    body: Optional[BodyData] = None


Server_Dynamic_Handler = Callable[[HandlerArgs, dict], Awaitable[HandlerResult]]
