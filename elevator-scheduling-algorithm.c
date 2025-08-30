#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>

int N,M,K,T;
key_t *solver_keys;
pthread_t threads[100];
int freeSolvers[100]; 
int maxLoad=5;

pthread_mutex_t qmutex;
pthread_mutex_t solvermutex;
pthread_mutex_t solverfree;

int taskq[100000];         
int qFront=0, qBack = 0; 
int remTasks=0;    

void addTask(int elevatorIndex){
    pthread_mutex_lock(&qmutex);
    taskq[qBack]=elevatorIndex;
    qBack++;
    remTasks++;
    pthread_mutex_unlock(&qmutex);
    //printf("7\n");
}

typedef struct {
    int requestId;
    int startFloor;
    int requestedFloor;
} PassengerRequest;

typedef struct {
    char authStrings[100][21];
    char elevatorMovementInstructions[100];
    PassengerRequest newPassengerRequest[30];
    int elevatorFloors[100];
    int droppedPassengers[1000];
    int pickedUpPassengers[1000][2];
} MainSharedMemory;

MainSharedMemory *shmPtr;

typedef struct TurnChangeResponse {
    long mtype;
    int turnNumber;
    int newPassengerRequestCount;
    int errorOccured;
    int finished;
} TurnChangeResponse;

typedef struct TurnChangeRequest {
    long mtype;
    int droppedPassengersCount;
    int pickedUpPassengersCount;
} TurnChangeRequest;

typedef struct {
    long mtype;
    int elevatorNumber;
    char authStringGuess[21];
} SolverRequest;

typedef struct {
    long mtype;
    int guessIsCorrect;
} SolverResponse;

typedef struct QueueNode {
    PassengerRequest data;
    struct QueueNode* next;
} QueueNode;

typedef struct Queue {
    QueueNode* front;
    QueueNode* rear;
    int size;
} Queue;

Queue* createQueue(){
    Queue* q=(Queue*)malloc(sizeof(Queue));
    q->front=q->rear=NULL;
    q->size=0;
    return q;
}

void enqueue(Queue* q,PassengerRequest request){
    QueueNode* newNode=(QueueNode*)malloc(sizeof(QueueNode));
    newNode->data=request;
    newNode->next=NULL;

    if(q->rear==NULL){
        q->front=q->rear=newNode;
    }else{
        q->rear->next=newNode;
        q->rear=newNode;
    }
    q->size++;
}

PassengerRequest dequeue(Queue* q) {
    if (q->front==NULL) {
        fprintf(stderr,"Queue is empty\n");
        exit(EXIT_FAILURE);
    }

    QueueNode* temp=q->front;
    PassengerRequest request=temp->data;
    q->front=q->front->next;

    if(q->front==NULL) {
        q->rear=NULL;
    }

    free(temp);
    q->size--;
    return request;
}

int isQueueEmpty(Queue* q) {
    return q->front==NULL;
}

int queueSize(Queue* q) {
    return q->size;
}

void freeQueue(Queue* q) {
    while(!isQueueEmpty(q)) {
        dequeue(q);
    }
    free(q);
}

PassengerRequest peek(Queue* q) {
    if (q->front==NULL) {
        fprintf(stderr,"Queue is empty, cannot peek\n");
        exit(EXIT_FAILURE);
    }
    return q->front->data;
}

typedef struct Elevator {
    int numPassengers;
    int isMoving;
    int currentFloor;
    int direction;
    Queue* pickupQueue;
    Queue* droppingQueue;
} Elevator;

Elevator* elevators[100];

Elevator* createElevator() {
    Elevator* elevator=(Elevator*)malloc(sizeof(Elevator));
    elevator->numPassengers=0;
    elevator->isMoving=0;
    elevator->currentFloor=0;
    elevator->direction=0;
    elevator->pickupQueue=createQueue();
    elevator->droppingQueue=createQueue();
    return elevator;
}

void freeElevator(Elevator* elevator) {
    freeQueue(elevator->pickupQueue);
    freeQueue(elevator->droppingQueue);
    free(elevator);
}

int generate_next_string(char *str,int length) {
    for (int i=length-1;i>= 0;i--) {
        if (str[i]<'f'){
            str[i]++;
            return 1;
        }else{
            str[i]='a';
        }
    }
    return 0;
}

int allelevatorsempty(Elevator* elevators[],int N){
    for(int i=0;i<N;i++){
        if(queueSize(elevators[i]->pickupQueue)!=0 || queueSize(elevators[i]->droppingQueue)!=0){
            return 0;
        }
    }
    return 1;
}

Queue* pendingQueue;
Queue* pendingQueueTemp;

int dropTemp(Elevator* elevator, int* droppedRequests){
    int count=0;
    if(queueSize(elevator->droppingQueue)>4){
        int farthestDropFloor;
        PassengerRequest farthestPassenger;
        int maxDist=INT_MIN;
        QueueNode* node=elevator->droppingQueue->front;
        int found=0;
        //printf("1\n");
        while(node!=NULL){
            if(!((elevator->direction==1 && elevator->currentFloor<node->data.requestedFloor) || (elevator->direction==-1 && elevator->currentFloor>node->data.requestedFloor))){
                int distance=abs(node->data.requestedFloor-elevator->currentFloor);
                if (distance>maxDist) {
                    found=1;
                    maxDist=distance;
                    farthestPassenger=node->data;
                    farthestDropFloor=node->data.requestedFloor;
                }
            }
            node=node->next;
        }
        //printf("2\n");
        if(!found){
            node=elevator->droppingQueue->front;
            maxDist=INT_MAX;
            while(node!=NULL){
            //if(!((elevator->direction==1 && elevator->currentFloor<node->data.requestedFloor) || (elevator->direction==-1 && elevator->currentFloor>node->data.requestedFloor))){
                int distance=abs(node->data.requestedFloor-elevator->currentFloor);
                if (distance<maxDist) {
                    found=1;
                    maxDist=distance;
                    farthestPassenger=node->data;
                    farthestDropFloor=node->data.requestedFloor;
                }
                node=node->next;
           // }
            }
        }
        //printf("3\n");
        Queue* tempQueue=createQueue();

        while(!isQueueEmpty(elevator->droppingQueue)) {
            PassengerRequest req=dequeue(elevator->droppingQueue);
            if (req.requestId==farthestPassenger.requestId){
                elevator->numPassengers--;
                droppedRequests[count]=req.requestId;
                count++;
            }else{
                enqueue(tempQueue,req);
            }
        }
        while(!isQueueEmpty(tempQueue)){
            enqueue(elevator->droppingQueue,dequeue(tempQueue));
        }
        farthestPassenger.startFloor=elevator->currentFloor;
        enqueue(pendingQueueTemp,farthestPassenger);
        //printf("Passenger %d dropped\n",farthestPassenger.requestId);
    }
    return count;
}

void pendingPassengersAssign(Elevator* elevator){
    int nearestPendingAssign=-1;
    while(queueSize(pendingQueue)>0){
        //for(int i=0;i<N;i++){
            int minPendingDistance=INT_MAX;
            PassengerRequest minDistPendingPassenger;
            if(queueSize(elevator->droppingQueue)+queueSize(elevator->pickupQueue)>maxLoad){
                return;
            }
            QueueNode* node=pendingQueue->front;
            int found=0;
            while(node!=NULL){
                int distance=abs(node->data.startFloor-elevator->currentFloor);
                if((elevator->direction==1 && elevator->currentFloor<node->data.startFloor) || (elevator->direction==-1 && elevator->currentFloor>node->data.startFloor) || (elevator->direction==0)){
                    if (distance<minPendingDistance){
                        found=1;
                        minPendingDistance=distance;
                        minDistPendingPassenger=node->data;
                    }
                }
                node=node->next;
            }
            
            if(!found) return;

            //Removing from pending passengers
            Queue* tempQueue=createQueue();

            while(!isQueueEmpty(pendingQueue)){
                PassengerRequest req=dequeue(pendingQueue);
                if (req.requestId==minDistPendingPassenger.requestId){

                }else{
                    enqueue(tempQueue,req);
                }
            }
            while(!isQueueEmpty(tempQueue)){
                enqueue(pendingQueue,dequeue(tempQueue));
            }

            //if(minDistPendingPassenger){
           // printf("Passenger %d assigned\n",minDistPendingPassenger.requestId);
            enqueue(elevator->pickupQueue,minDistPendingPassenger);
            //}
        //}
    }
}

int elevatorAssign(Elevator* elevators[],PassengerRequest newPassengerRequest){
    int nearestElevatorIndex=-1;
    int minDistance=INT_MAX;

    for(int i=0;i<N;i++){
        if(queueSize(elevators[i]->droppingQueue)+queueSize(elevators[i]->pickupQueue)>maxLoad+3){
            continue;
        }
        int distance=abs(elevators[i]->currentFloor-newPassengerRequest.startFloor);
        if((elevators[i]->direction==1 && elevators[i]->currentFloor<newPassengerRequest.startFloor) || (elevators[i]->direction==-1 && elevators[i]->currentFloor>newPassengerRequest.startFloor) || (elevators[i]->direction==0)){
            if(distance<minDistance){
                minDistance=distance;
                nearestElevatorIndex=i;
            }
        }
    }
    
    // if(nearestElevatorIndex==-1){
    //     int minPassengers=INT_MAX;
    //     for(int i=0;i<N;i++){
    //         //int distance=abs(elevators[i]->currentFloor-newPassengerRequest.startFloor);
    //         int numPassengers=queueSize(elevators[i]->droppingQueue) + queueSize(elevators[i]->pickupQueue);
    //         if(numPassengers>6){
    //             continue;
    //         }
    //         if(numPassengers<minPassengers){
    //             minPassengers=numPassengers;
    //             nearestElevatorIndex=i;
    //         }
    //     }
    // }
    // if(nearestElevatorIndex==-1){
    //     for(int i=0;i<N;i++){
    //         int distance=abs(elevators[i]->currentFloor-newPassengerRequest.startFloor);
    //         int numPassengers=queueSize(elevators[i]->droppingQueue);
    //         if(distance<minDistance){
    //             minDistance=distance;
    //             nearestElevatorIndex=i;
    //         }
    //     }
    // }

    if (nearestElevatorIndex!=-1){
        enqueue(elevators[nearestElevatorIndex]->pickupQueue,newPassengerRequest);
        elevators[nearestElevatorIndex]->numPassengers++;
        return nearestElevatorIndex;
        //printf("Assigned request %d to elevator %d\n",newPassengerRequest.requestId,nearestElevatorIndex);
    }else{
        enqueue(pendingQueue,newPassengerRequest);
        return -1;
        //fprintf(stderr, "No elevator available to assign the request.\n");
    }
}



int updateElevatorDirection(Elevator* elevator) {
    int pickupDistance=INT_MAX;
    int dropDistance= INT_MAX;
    int closestPickupFloor=-1;
    int closestDropFloor=-1;

    QueueNode* node=elevator->droppingQueue->front;
    while(node!=NULL){
        int distance=abs(node->data.requestedFloor-elevator->currentFloor);
        if ((elevator->direction==0 || (elevator->direction==-1 && node->data.requestedFloor<elevator->currentFloor) || (elevator->direction==1 && node->data.requestedFloor>elevator->currentFloor)) && distance<dropDistance) {
            dropDistance=distance;
            closestDropFloor=node->data.requestedFloor;
        }
        node=node->next;
    }
    
    if(dropDistance==INT_MAX){
        node=elevator->pickupQueue->front;
        while(node!=NULL){
            int distance=abs(node->data.startFloor-elevator->currentFloor);
            if ((elevator->direction==0 || (elevator->direction==-1 && node->data.startFloor<elevator->currentFloor) || (elevator->direction==1 && node->data.startFloor>elevator->currentFloor)) && distance<pickupDistance) {
                pickupDistance=distance;
                closestPickupFloor=node->data.startFloor;
            }
            node=node->next;
        }
    }

    if (pickupDistance==INT_MAX && dropDistance==INT_MAX) {
        elevator->direction = 0;
    } else if (pickupDistance<=dropDistance) {
        if (closestPickupFloor>elevator->currentFloor) {
            elevator->direction=1;
        }else if(closestPickupFloor<elevator->currentFloor) {
            elevator->direction=-1;
        }else{
            elevator->direction=0;
        }
    }else{
        if(closestDropFloor>elevator->currentFloor){
            elevator->direction=1;
        }else if(closestDropFloor<elevator->currentFloor){
            elevator->direction=-1;
        }else{
            elevator->direction=0;
        }
    }
    return elevator->direction;
}

int pickupPeople(Elevator* elevator, int pickedUpRequests[][2], int elevatorNumber) {
    int count = 0;
    Queue* tempQueue=createQueue();

    while(!isQueueEmpty(elevator->pickupQueue)){
        PassengerRequest req=dequeue(elevator->pickupQueue);
        if (req.startFloor==elevator->currentFloor) {
            if(!(req.requestedFloor==elevator->currentFloor)){
                elevator->numPassengers++;
                enqueue(elevator->droppingQueue,req);
                pickedUpRequests[count][0]=req.requestId;
                pickedUpRequests[count][1]=elevatorNumber;
                count++;
            }
        } else {
            enqueue(tempQueue,req);
        }
    }
    while (!isQueueEmpty(tempQueue)){
        enqueue(elevator->pickupQueue,dequeue(tempQueue));
    }

    freeQueue(tempQueue);
    return count;
}


int dropPeople(Elevator* elevator,int droppedRequests[]) {
    int count=0;
    Queue* tempQueue=createQueue();

    while(!isQueueEmpty(elevator->droppingQueue)) {
        PassengerRequest req=dequeue(elevator->droppingQueue);

        if (req.requestedFloor==elevator->currentFloor) {
            elevator->numPassengers--;
            droppedRequests[count]=req.requestId;
            count++;
        } else {
            enqueue(tempQueue,req);
        }
    }
    while(!isQueueEmpty(tempQueue)){
        enqueue(elevator->droppingQueue,dequeue(tempQueue));
    }

    freeQueue(tempQueue);
    return count;
}

int getFreeSolver(){
        for(int i=0;i<M;i++){
            if(freeSolvers[i]==1){ 
                freeSolvers[i]=0;
                return i;              
            }
        }
        return -1;
}

void* guess(void* arg) {
    //while(1){
        int solverIndex=(int)arg;

        int i;
        pthread_mutex_lock(&qmutex);
        // if(remTasks==0) {
        //     break;
        // }
        if (remTasks<=0) {
            pthread_mutex_unlock(&qmutex);
            pthread_exit(NULL);
        }
        i=taskq[qFront];
        qFront++;
        remTasks--;
        pthread_mutex_unlock(&qmutex);
        
        key_t solver_msgkey = solver_keys[solverIndex];
        //printf("Thread %d assigned to elevator %d\n",solverIndex,i);
        //key_t solver_msgkey=solver_keys[i%M];  // --------
        int solver_msgid=msgget(solver_msgkey,IPC_CREAT|0666);

        if (solver_msgid==-1) {
            perror("msgget");
            pthread_exit(NULL);
        }

        SolverRequest request1,request2;
        SolverResponse response;
        int stringLength=queueSize(elevators[i]->droppingQueue);
        memset(request2.authStringGuess,'a',stringLength);
        request2.authStringGuess[stringLength]='\0';
        while(1){
            request1.mtype=2;
            request1.elevatorNumber=i;
            if(msgsnd(solver_msgid,&request1,sizeof(request1)-sizeof(long),0)==-1){
                perror("msgsnd");
                break;
            }

            request2.mtype=3;
            if(msgsnd(solver_msgid,&request2,sizeof(request2)-sizeof(long),0)==-1){
                perror("msgsnd");
                break;
            }

            if(msgrcv(solver_msgid,&response,sizeof(response)-sizeof(long),4,0)==-1){
                perror("msgrcv here");
                break;
            }

            if(response.guessIsCorrect==1){
                for(int j=0;j<stringLength;j++){
                    shmPtr->authStrings[i][j]=request2.authStringGuess[j];
                }
                shmPtr->authStrings[i][stringLength]='\0';
                printf("Correct authString found for elevator %d: %s\n",i,request2.authStringGuess);
                break;
            }

            if(!generate_next_string(request2.authStringGuess, stringLength)){
                printf("All combinations tried without finding the target for elevator %d.\n",i);
                break;
            }
        }
        //printf("Thread %d finished exec\n",solverIndex);
        //pthread_mutex_lock(&solverfree);
        freeSolvers[solverIndex]=1;
        //pthread_mutex_unlock(&solverfree);
        pthread_exit(NULL);
}

// void guess(int arg,int i) {
//     //while(1){
//         int solverIndex=(int)arg;

        
//         key_t solver_msgkey = solver_keys[solverIndex];
//         //printf("Thread %d assigned to elevator %d\n",solverIndex,i);
//         //key_t solver_msgkey=solver_keys[i%M];  // --------
//         int solver_msgid=msgget(solver_msgkey,IPC_CREAT|0666);

//         if (solver_msgid==-1) {
//             perror("msgget");
//             pthread_exit(NULL);
//         }

//         SolverRequest request1,request2;
//         SolverResponse response;
//         int stringLength=queueSize(elevators[i]->droppingQueue);
//         memset(request2.authStringGuess,'a',stringLength);
//         request2.authStringGuess[stringLength]='\0';
//         while(1){
//             request1.mtype=2;
//             request1.elevatorNumber=i;
//             if(msgsnd(solver_msgid,&request1,sizeof(request1)-sizeof(long),0)==-1){
//                 perror("msgsnd");
//                 break;
//             }

//             request2.mtype=3;
//             if(msgsnd(solver_msgid,&request2,sizeof(request2)-sizeof(long),0)==-1){
//                 perror("msgsnd");
//                 break;
//             }

//             if(msgrcv(solver_msgid,&response,sizeof(response)-sizeof(long),4,0)==-1){
//                 perror("msgrcv here");
//                 break;
//             }

//             if(response.guessIsCorrect==1){
//                 for(int j=0;j<stringLength;j++){
//                     shmPtr->authStrings[i][j]=request2.authStringGuess[j];
//                 }
//                 shmPtr->authStrings[i][stringLength]='\0';
//                 printf("Correct authString found for elevator %d: %s\n",i,request2.authStringGuess);
//                 break;
//             }

//             if(!generate_next_string(request2.authStringGuess, stringLength)){
//                 printf("All combinations tried without finding the target for elevator %d.\n",i);
//                 break;
//             }
//         }
//         //printf("Thread %d finished exec\n",solverIndex);
//         //pthread_mutex_lock(&solverfree);
//       // freeSolvers[solverIndex]=1;
//         //pthread_mutex_unlock(&solverfree);
//       // pthread_exit(NULL);
// }

void rotateArr(int* arr) {
    if (N<=1) return;
    int first=arr[0]; 
    for(int i=0;i<N-1;i++) {
        arr[i]=arr[i+1]; 
    }
    arr[N-1]=first;
}

int main() {
    if(pthread_mutex_init(&qmutex, NULL)!=0){
        printf("Failed to initialize queue mutex\n");
        return 1;
    }

    if(pthread_mutex_init(&solvermutex, NULL)!=0){
        printf("Failed to initialize solver mutex\n");
        return 1;
    }

    if(pthread_mutex_init(&solverfree, NULL)!=0){
        printf("Failed to initialize solver mutex\n");
        return 1;
    }

    FILE *file=fopen("input.txt","r");
    if(!file){
        perror("Error opening file");
        return -1;
    }

    pendingQueue=createQueue();
    pendingQueueTemp=createQueue();

    //int N, K, M, T;
    key_t shm_key, main_msgq_key;

    if (fscanf(file,"%d %d %d %d",&N,&K,&M,&T)!=4 || fscanf(file,"%d %d",&shm_key,&main_msgq_key)!=2){
        fclose(file);
        return -1;
    }

    solver_keys=malloc(M*sizeof(key_t));
    if(!solver_keys) {
        fclose(file);
        return -1;
    }
    //printf("%d",M);

    for (int i=0;i<M;i++) {
        if (fscanf(file,"%d",&solver_keys[i])!=1) {
            free(solver_keys);
            fclose(file);
            return -1;
        }
    }

    for (int i=0;i<N;i++) {
        elevators[i]=createElevator();
        //elevators[i]->currentFloor = 0;
    }

    int shmId=shmget(shm_key,sizeof(MainSharedMemory),IPC_CREAT|0666);
    if (shmId<0){
        perror("Shared memory creation failed");
        exit(1);
    }

    shmPtr=(MainSharedMemory *)shmat(shmId, NULL, 0);
    if (shmPtr==(void *)-1) {
        perror("Shared memory attachment failed");
        exit(1);
    }

    int msgId=msgget(main_msgq_key,IPC_CREAT|0666);
    if (msgId<0){
        perror("Message queue creation failed");
        exit(1);
    }

    TurnChangeResponse msg;
    if (msgrcv(msgId,&msg,sizeof(msg)-sizeof(long),2,0)<0) {
        perror("Message receiving failed");
        exit(1);
    }

    int freeSolver[M];
    for(int i=0;i<M;i++){
        freeSolver[i]=1;
    }
    
    printf("NUMBER OF SOLVERS: %d\n",M);

    for(int i=0;i<M;i++){
        freeSolvers[i]=1;
    }
    
    PassengerRequest reqarr[3000];
    int reqarridx=0;

    int* elevatorIndexes=malloc(N*sizeof(int));
    for(int i=0;i<N;i++){
        elevatorIndexes[i]=i;
    }

    while(!msg.finished){
        printf("Turn Number: %d\n",msg.turnNumber);
        //printf("T: %d\n",T);
        // int currturn=msg.turnNumber;
        // printf("Number of new requests: %d\n",msg.newPassengerRequestCount);
        // AuthString Part
        int solversUsed=0;
        int pickcnt=0;
        int droppedcnt=0;
        if(msg.turnNumber>T){
            // for(int i=0;i<N;i++){
            //     if(queueSize(elevators[i]->droppingQueue)>0){
            //         //movingElevatorsCnt++;
            //         int solverIndex=0;
            //         guess(solverIndex,i);
            //         //printf("authstring needed");
            //         //addTask(i);
            //     }
            // }
            
            int movingElevatorsCnt=0;
            for(int i=0;i<N;i++){
                //printf("Checking Moving Elevators....\n");
                if(queueSize(elevators[i]->droppingQueue)>0){
                    movingElevatorsCnt++;
                    //printf("authstring needed");
                    addTask(i);
                }
            }
            
          // printf("1\n");
            int taskcnt=remTasks;
            //printf("Assigning Threads....\n");
            for(int i=0;i<movingElevatorsCnt;i++){  //issue is ki M baar hi loop chl rha hai, so it is handling only M elevators at a time
                int solverIndex=-1;
                while(solverIndex==-1){
                    pthread_mutex_lock(&solvermutex);
                    //printf("1\n");
                    solverIndex = getFreeSolver();
                    pthread_mutex_unlock(&solvermutex);
                    if(solverIndex==-1){
                        printf("waiting....\n");
                        usleep(10);
                    }
                }
                //printf("Free Solver Found....\n");
                if(solverIndex==-1){
                    printf("-1\n");
                }
                if(taskcnt>0){
                    if(pthread_create(&threads[solverIndex],NULL,guess,(void*)solverIndex)!=0) {
                        perror("pthread_create");
                        exit(1);
                    }
                    pthread_join(threads[solverIndex], NULL);
                    taskcnt--;
                }else{
                    printf("No available tasks....\n");
                }
            }
            
            // for(int i = 0; i < M; i++) {
            //     pthread_join(threads[i], NULL);
            // }
            //printf("Threads joined....\n");
            
        // printf("2\n");
            
            // updating the floors
            for(int i=0;i<N;i++){
                elevators[i]->currentFloor=shmPtr->elevatorFloors[i];
                //printf("current floor: %d\n",elevators[i]->currentFloor);
            }

            int allpickups[100][2];
            int allpickupsidx=0;
            int alldrops[100];
            int alldropsidx=0;

            while(queueSize(pendingQueueTemp)!=0){
                PassengerRequest req=dequeue(pendingQueueTemp);
                enqueue(pendingQueue,req);
            }
            
            // int pendingQueueSize=queueSize(pendingQueue);
            // Queue* tempQueue=createQueue();
            // for (int i=0;i<pendingQueueSize;i++){
            //     PassengerRequest req=dequeue(pendingQueue);
            //     //printf("Request %d: Start Floor = %d, Requested Floor = %d\n", shmPtr->newPassengerRequest[i].requestId, shmPtr->newPassengerRequest[i].startFloor, shmPtr->newPassengerRequest[i].requestedFloor);
            //     elevatorAssign(elevators,req);
            // }
            for (int i=0;i<N;i++) {
                int droppedRequests[20]; 
                int droppedCount=dropPeople(elevators[i],droppedRequests);
                droppedcnt+=droppedCount;
            // printf("Elevator %d dropped off %d passengers on floor %d\n", i, droppedCount, elevators[i]->currentFloor);
                for(int j=0; j<droppedCount;j++){
                    alldrops[alldropsidx++]=droppedRequests[j];
                // printf("Dropped requestId %d\n",droppedRequests[j]);
                }
            }
            
            
            for(int i=0;i<N;i++){
                int droppedRequests[20]; 
                if(queueSize(elevators[i]->droppingQueue)>4){
                    int droppedCount=dropTemp(elevators[i],droppedRequests);
                    droppedcnt+=droppedCount;
                    // printf("Elevator %d dropped off %d passengers on floor %d\n", i, droppedCount, elevators[i]->currentFloor);
                    for(int j=0; j<droppedCount;j++){
                        alldrops[alldropsidx++]=droppedRequests[j];
                    // printf("Dropped requestId %d\n",droppedRequests[j]);
                    }
                }
            }

            // Assigning elevators for each request
            // for (int i=0;i<msg.newPassengerRequestCount;i++){
            //     //printf("Request %d: Start Floor = %d, Requested Floor = %d\n", shmPtr->newPassengerRequest[i].requestId, shmPtr->newPassengerRequest[i].startFloor, shmPtr->newPassengerRequest[i].requestedFloor);
            //     elevatorAssign(elevators,shmPtr->newPassengerRequest[i]);
            // }
            //printf("chkpt1\n");
            // Pick and drop at this turn for all the elevators

            //printf("chkpt2\n");
            
             for(int i=0;i<N;i++){
                Elevator* tempelev=elevators[elevatorIndexes[0]];
                pendingPassengersAssign(tempelev);
                rotateArr(elevatorIndexes);
            }

            for(int i=0;i<N;i++){
                Queue* tempQueue=createQueue();
                while(!isQueueEmpty(elevators[i]->pickupQueue)){
                    PassengerRequest req=dequeue(elevators[i]->pickupQueue);

                    if(elevatorAssign(elevators,req)!=i){

                    }else{
                        enqueue(tempQueue,req);
                    }
                }
                while(!isQueueEmpty(tempQueue)){
                    enqueue(elevators[i]->pickupQueue,dequeue(tempQueue));
                }
                freeQueue(tempQueue);
            }
            
            
            for(int i=0;i<N;i++){
                int pickedUpRequests[20][2];
                int pickedUpCount=pickupPeople(elevators[i],pickedUpRequests,i);
                pickcnt+=pickedUpCount;
                //printf("Elevator %d picked up %d passengers on floor %d\n",i,pickedUpCount,elevators[i]->currentFloor);
                for (int j=0;j<pickedUpCount;j++) {
                    allpickups[allpickupsidx][0]=pickedUpRequests[j][0];
                    allpickups[allpickupsidx++][1]=pickedUpRequests[j][1];
                //  printf("Picked up requestId %d by elevator %d\n",pickedUpRequests[j][0],pickedUpRequests[j][1]);
                }
            }
            //printf("chkpt3\n");
            
            //printf("\npickups\n");
            for(int i=0;i<allpickupsidx;i++){
                shmPtr->pickedUpPassengers[i][0]=allpickups[i][0];
                shmPtr->pickedUpPassengers[i][1]=allpickups[i][1];
                //printf("pickups, drops : %d %d\n",allpickups[i][0],allpickups[i][1]);
            }
            //printf("\ndrops\n");
            for(int i=0;i<alldropsidx;i++){
                shmPtr->droppedPassengers[i]=alldrops[i];
            //  printf("%d\n",alldrops[i]);
            }
        }else{
            for(int i=0;i<msg.newPassengerRequestCount;i++){
                enqueue(pendingQueue,shmPtr->newPassengerRequest[i]);
            }
        }

        for(int i=0;i<N;i++){
            int d=updateElevatorDirection(elevators[i]);
            if(d==1){
                shmPtr->elevatorMovementInstructions[i]='u';
            }else if(d==-1){
                shmPtr->elevatorMovementInstructions[i]='d';
            }else{
                shmPtr->elevatorMovementInstructions[i]='s';
            }
            //printf("%c\n",shmPtr->elevatorMovementInstructions[i]);
        }
       // printf("Rem Size: %d\n",queueSize(pendingQueue));

        TurnChangeRequest request={.mtype=1,.droppedPassengersCount=droppedcnt,.pickedUpPassengersCount=pickcnt};
        if (msgsnd(msgId,&request,sizeof(TurnChangeRequest)-sizeof(long),0)==-1) {
            perror("Error sending message here");
            break;
        }

        if (msgrcv(msgId,&msg,sizeof(msg)-sizeof(long),2,0)<0){
            perror("Message receiving failed");
            exit(1);
        }
    }

    pthread_mutex_destroy(&qmutex);
    pthread_mutex_destroy(&solvermutex);
    pthread_mutex_destroy(&solverfree);

    shmdt(shmPtr);

    return 0;
}