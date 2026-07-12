\paragraph{Data Structure.}
Our queue nodes embed a \texttt{status} and a customizable \texttt{payload} defined by specific use cases.
% We eliminate the complex per-thread return arrays (\texttt{returnedValues}) and \texttt{deqThreadId} CAS operations from the original DurableQueue, as we strictly have a single consumer per queue.

\begin{tcolorbox}[colback=white,colframe=black!60,boxrule=0.5pt,left=6pt,right=6pt,top=4pt,bottom=4pt]
\small
\textbf{Unified Data Structure for \sys Queue.}\\
\texttt{Node \{}\\
\qquad\texttt{Node* next;}\\
\qquad\texttt{char status $\in$ \{INIT, DOING, DONE, CRASH\};}\\
\qquad\texttt{Payload payload; \emph{\blue{/* Defined by specific use cases (IPC/Sched/Noti) */}}}\\
\texttt{\}}\\
\texttt{Queue \{ Node* head; Node* tail; \}}
\end{tcolorbox}

\paragraph{Enqueue (Producer).}
The enqueue operation (Algorithm~\ref{alg:appendix-queue-enq}) remains lock-free. 
A producer:
\begin{enumerate}
    \item Allocates a node with \texttt{status = INIT}, embeds the request in the \texttt{payload}, and \texttt{FLUSH(node)} (Line1).
    \item Link it into the list with CAS (Line6).
    \item Flush the linking pointer (Line7).
    \item Change \texttt{tail} to the new node (Line8).
\end{enumerate}
% The lock-free nature ensures that if a producer crashes mid-enqueue, other producers can help finish the linking (Lines 11-12) and continue their own operations.
% allocates a node, initializes it with \texttt{INIT}, embeds the request in the \texttt{payload}, and links it to the tail using Compare-And-Swap (CAS). 
The lock-free nature ensures that if a producer crashes mid-enqueue, other producers can help finish the linking (Lines 11-12) and continue their own operations.

\begin{itemize}
    \item \textbf{Linearization point:} successful \texttt{CAS(\&last->next, next, node)}.
    \item \textbf{Crash tolerance (producer):} if an enqueuer fails after publishing \texttt{last->next} but before advancing \texttt{tail}, other producers will help advance \texttt{tail}.
\end{itemize}

\begin{algorithm}[h]
\caption{\texttt{enq(payload)} for \sys.}
\label{alg:appendix-queue-enq}
\small
\DontPrintSemicolon
\texttt{node $\leftarrow$ new Node(INIT, payload); FLUSH(node)}\;
\While{true}{
  \texttt{last $\leftarrow$ tail; next $\leftarrow$ last->next}\;
  \If{\texttt{last == tail}}{
    \If{\texttt{next == NULL}}{
      \If{\texttt{CAS(\&last->next, next, node)}}{
        \texttt{FLUSH(\&last->next)}\;
        \texttt{CAS(\&tail, last, node)}\;
        \Return node\;
      }
    }
    \Else{
      \emph{\blue{/* Help crashed enqueuer */}}\;
      \texttt{FLUSH(\&last->next)}\;
      \texttt{CAS(\&tail, last, next)}\;
    }
  }
}
\end{algorithm}

\paragraph{Dequeue (Consumer).}
The dequeue operation (Algorithm~\ref{alg:appendix-queue-deq}) is executed exclusively by the single consumer (e.g., the target CPU core). When queue is not empty, the consumer:
\begin{enumerate}
    \item Dequeues the node (Line9).
    \item Transitions its status from \texttt{INIT} to \texttt{DOING} (Line10).
    \item Processes the payload via a use-case-specific \texttt{handle()} function (Line12).
    \item Marks it as \texttt{DONE} (Line13) and persists it (Line14).
\end{enumerate}

Line7 fix inconsistency caused by enqueuer crash.

\begin{itemize}
    % \item \textbf{Single-consumer simplification:} no need for per-thread returned-value arrays or consumer-thread IDs.
    \item \textbf{Detectability for producers:} producers can observe \texttt{status == DONE} as a completion signal.
\end{itemize}
\
\begin{algorithm}[h]
\caption{\texttt{deq()} for \sys.}
\label{alg:appendix-queue-deq}
\small
\DontPrintSemicolon
\While{true}{
  \texttt{first $\leftarrow$ head; last $\leftarrow$ tail; next $\leftarrow$ first->next}\;
  \If{\texttt{first == head}}{
    \If{\texttt{first == last}}{
      \If{\texttt{next == NULL}} { \Return \texttt{NULL} }
      \texttt{FLUSH(\&last->next); CAS(\&tail, last, next)}\;
    }
    \Else{
        \texttt{n $\leftarrow$ next}\;
        \If{\texttt{CAS(\&n->status, INIT, DOING)}}{
            \texttt{FLUSH(\&n->status)}\;
            \texttt{handle(n)} \emph{\blue{/* Varies by use case (IPC/Sched/Noti) */}}\;
            \texttt{n->status $\leftarrow$ DONE}\; 
            \texttt{FLUSH(\&n->status)}\;
        }
        \texttt{CAS(\&head, first, next)}\;
        \Return\;
    }
  }
}
\end{algorithm}

\subsubsection{Failure Model and Recovery Semantics}
\label{sec:appendix-failure-model}

The primary motivation for using this lock-free persistent queue is to tolerate \textbf{Enqueuer Crashes} without stalling the entire system, while simplifying \textbf{Dequeuer Crashes} based on physical machine boundaries.

\paragraph{Enqueuer Crash.}
An enqueuer crash occurs when the machine hosting the producer fails. 
Thanks to the lock-free linking (Algorithm~\ref{alg:appendix-queue-enq}), an enqueuer crashing mid-operation will \emph{never} block the queue. Other surviving machines can still enqueue messages (IPC), schedule threads (Scheduler), or wait for events (Notification) on this queue. If the enqueuer crashes after the commit point (Line 7), the request will be processed normally by the consumer.
% \begin{itemize}
%     \item \textbf{Safety:} incomplete enqueue cannot corrupt the queue structure.
%     \item \textbf{Liveness:} other producers can complete the linkage and make progress.
%     \item \textbf{After commit:} the consumer will eventually observe and process the request.
% \end{itemize}

\paragraph{Dequeuer Crash (Consumer Failure).}
In \sys, a dequeuer is firmly bound to a specific physical CPU core. Therefore, a dequeuer crash implies that the entire physical machine hosting this queue has failed.
Because the machine's volatile execution context (registers, memory) is lost, attempting to resume partially processed requests (\texttt{DOING}) upon reboot is generally unsafe and unnecessary.

Thus, our recovery semantics for dequeuer crashes are straightforward: Upon machine reboot, the queue is completely cleared and re-initialized.
\begin{itemize}
    \item \textbf{Why clear the queue?} For the Scheduler and Notification, the threads associated with the queued nodes were running on the crashed machine and are now dead; their contexts are lost. For IPC, if a file system operation was interrupted, the underlying persistent file system will perform our recovery method. The IPC queue itself acts merely as a transport layer and does not need to replay messages.
    \item \textbf{What is the purpose of INIT/DOING/DONE/CRASH?} INIT/DOING/DONE are used for detectable execution for the Enqueuer. The enqueuer spins on \texttt{n->status}; once it observes \texttt{DONE}, it knows the operation is complete and can safely read the response (e.g., IPC return values). CRASH is used for dequeuer crash recovery.
\end{itemize}

\subsubsection{IPC Queue}
\label{sec:appendix-ipc}
For inter-machine IPC, the queue serves as a message channel. The producer allocates a variable-sized node to accommodate the \texttt{msgBuffer}. After enqueuing, the producer spins on \texttt{n->status}. 
Once the consumer executes the message and sets \texttt{status = DONE}, the producer reads the response from \texttt{msgBuffer}.
The consumer handles the message based on the \texttt{msgType}, it writes the response back to \texttt{msgBuffer}, and persists the \texttt{msgBuffer} during \texttt{handle(n)}.
% If the consumer machine crashes, the queue is cleared upon reboot, and the producer's IPC call will return a failure code (handled by application-level retries).
\begin{itemize}
    \item \textbf{Payload:} \texttt{msgType}, \texttt{msgBuffer} (variable-sized).
    \item \textbf{Queue:} \texttt{IPCQueue ipcQueue[MAX\_MACHINE\_NUM]}.
    \item \textbf{Producer:} applications or system services that invoke IPC.
    % , producer waits for \texttt{status == DONE} then reads \texttt{msgBuffer}.
    \item \textbf{Consumer:} each physical machine's dedicated polling CPU.
    \item \textbf{Producer crash:} IPC calls are normally executed by consumer, producer checks \texttt{status == DONE} to detect completion of message, and retry or abort based on application semantics.
    \item \textbf{Consumer crash:} queue reset on reboot, marks all in-flight nodes in the queue as \texttt{status = CRASH}; caller observes failure and retries at the application layer.
\end{itemize}