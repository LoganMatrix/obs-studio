<?php

class LiveViewModel extends ViewModel {
    public $viewFields = array(
        'Room'=>array('*','_type'=>'LEFT'), //Must be LEFT 返回左表所有数据，即使右表没有匹配的
        'Agent'=>array('agent_id','name'=>'agent_name','_on'=>'Agent.agent_id=Room.agent_id','_type'=>'LEFT'),
        'Subject'=>array('subject_id','subject_name','_on'=>'Subject.subject_id=Room.subject_id')
    );
}
?>