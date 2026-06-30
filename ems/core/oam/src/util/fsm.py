from dataclasses import dataclass
from enum import Enum, auto
from typing import Callable, Dict, Generic, Optional, Tuple, TypeVar, Any, List


S = TypeVar("S", bound=Enum)  # State Enum
E = TypeVar("E", bound=Enum)  # Event Enum
P = TypeVar("P")
Action = Callable[[S, P], None]
Guard  = Callable[[S, P], bool]
Hook   = Callable[[Enum, Optional[P]], None]  # state/event, payload


@dataclass(frozen=True)
class Transition(Generic[S, P]):
    next_state: S
    action: Optional[Action[S, Optional[P]]] = None   # next-state, payload
    guard: Optional[Guard[S, Optional[P]]] = None     # next-state, payload


class FSM(Generic[S, E, P]):
    def __init__(
        self,
        initial_state: S,
        transitions: Dict[Tuple[S, E], Transition[S, P]], # key:(current_state, event), value:Transition(next_state, action, guard)
        *,
        on_enter: Optional[Hook] = None,
        on_exit: Optional[Hook] = None,
        on_unknown: Optional[Callable[[E, Optional[P]], None]] = None,
    ):
        self._state: S = initial_state
        self._transitions: Dict[Tuple[S, E], Transition[S, P]] = transitions
        self._on_enter: Optional[Hook] = on_enter
        self._on_exit: Optional[Hook] = on_exit
        self._on_unknown: Optional[Callable[[E, Optional[P]], None]] = on_unknown

        if self._on_enter:
            self._on_enter(self._state, None)

    @property
    def state(self) -> S:
        return self._state

    def step(self, event: E, payload: Optional[P] = None) -> S:
        # find next state
        spec = self._transitions.get((self._state, event))
        if spec is None:
            if self._on_unknown:
                self._on_unknown(event, payload)
            return self._state

        # guard
        if spec.guard is not None and not spec.guard(spec.next_state, payload):
            if self._on_unknown:
                self._on_unknown(event, payload)
            return self._state

        # exit -> action -> enter
        if self._on_exit:
            self._on_exit(self._state, payload)

        # action
        if spec.action:
            spec.action(spec.next_state, payload)

        # change next state
        self._state = spec.next_state

        if self._on_enter:
            self._on_enter(self._state, payload)

        return self._state

    def isStep(self, event: E, payload: Optional[P] = None) -> bool:
        spec = self._transitions.get((self._state, event))
        if not spec:
            return False
        return True if spec.guard is None else bool(spec.guard(spec.next_state, payload))


if __name__ == "__main__":

    class State(Enum):
        NEW = auto()
        PAID = auto()
        SHIPPED = auto()
        DONE = auto()
        CANCELLED = auto()


    class Event(Enum):
        PAY = auto()
        SHIP = auto()
        DELIVER = auto()
        CANCEL = auto()

    Payload = Dict[str, Any]

    def example() -> None:
        log: List[str] = []

        def on_enter(state: State, payload: Optional[Payload]):
            log.append(f"[enter] {state.name} payload={payload}")

        def on_exit(state: State, payload: Optional[Payload]):
            log.append(f"[exit]  {state.name} payload={payload}")

        def on_unknown(event: Event, payload: Optional[Payload]):
            log.append(f"[unknown] event={event.name} payload={payload}")

        # 액션들
        def charge(next_state: State, _payload: Optional[Payload]) -> None:
            log.append(f"{State}, 액션: 결제 승인")

        def ship(next_state: State, _payload: Optional[Payload]) -> None:
            log.append(f"{State}, 액션: 출고")

        def deliver(next_state: State, _payload: Optional[Payload]) -> None:
            log.append(f"{State}, 액션: 배송 완료 기록")

        # 가드
        def cancellable(next_state: State, payload: Optional[Payload]) -> bool:
            return not (payload and payload.get("after_ship") is True)

        transitions: Dict[Tuple[State, Event], Transition[State, Payload]] = {
            (State.NEW, Event.PAY): Transition(State.PAID, action=charge),
            (State.PAID, Event.SHIP): Transition(State.SHIPPED, action=ship),
            (State.SHIPPED, Event.DELIVER): Transition(State.DONE, action=deliver),
            (State.NEW, Event.CANCEL): Transition(State.CANCELLED, guard=cancellable),
            (State.PAID, Event.CANCEL): Transition(State.CANCELLED, guard=cancellable),
            # SHIPPED 이후 취소 전이는 정의하지 않음 → unknown 훅으로 처리
        }

        fsm = FSM[State, Event, Payload](
            initial_state=State.NEW,
            transitions=transitions,
            on_enter=on_enter,
            on_exit=on_exit,
            on_unknown=on_unknown,
        )

        fsm.step(Event.PAY)
        fsm.step(Event.CANCEL, {"after_ship": True})  # 가드 실패 → 상태 유지
        fsm.step(Event.SHIP)
        fsm.step(Event.CANCEL)  # 미정의 전이 → unknown
        fsm.step(Event.DELIVER)

        print("최종 상태:", fsm._state.name)
        print("\n".join(log))


    example()
