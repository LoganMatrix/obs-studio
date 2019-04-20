
import Dialog from '../../vant-weapp/dialog/dialog';

// 获取全局的app对象...
const g_app = getApp()

Page({

  /**
   * 页面的初始数据
   */
  data: {
    icon: {
      type: 'warn',
      color: '#ef473a',
    },
    buttons: [{
      type: 'balanced',
      block: true,
      text: '点击授权',
      openType: 'getUserInfo',
    }],
    m_code: '',
    m_bgColor: '#fff',
    m_show_auth: false,
  },

  /**
   * 生命周期函数--监听页面加载
   */
  onLoad: function (options) {
    let theAppData = g_app.globalData;
    // 如果用户编号和用户信息都是有效的，直接显示正常页面...
    if (theAppData.m_nUserID > 0 && theAppData.m_userInfo != null) {
      this.setData({ m_show_auth: 2 });
      return;
    }
    // 开始登录过程，加载动画...
    wx.showLoading({ title: '加载中' });
    let that = this;
    wx.login({
      success: res => {
        that.setData({ m_code: res.code });
        // 立即读取用户信息，第一次会弹授权框...
        wx.getUserInfo({
          lang: 'zh_CN',
          withCredentials: true,
          success: res => {
            console.log(res);
            wx.hideLoading();
            // 获取成功，通过网站接口获取用户编号...
            g_app.doAPILogin(that, that.data.m_code, res.userInfo, res.encryptedData, res.iv);
          },
          fail: res => {
            console.log(res);
            wx.hideLoading();
            // 获取失败，显示授权对话框...
            that.setData({ m_show_auth: true });
          }
        })
       }
    });
  },

  // 拒绝授权|允许授权之后的回调接口...
  getUserInfo: function (res) {
    // 注意：拒绝授权，也要经过这个接口，没有res.detail.userInfo信息...
    console.log(res.detail);
    // 保存this对象...
    var that = this
    // 允许授权，通过网站接口获取用户编号...
    if (res.detail.userInfo) {
      g_app.doAPILogin(that, that.data.m_code, res.detail.userInfo, res.detail.encryptedData, res.detail.iv)
    }
  },

  // 响应登录错误接口...
  onLoginError: function(inTitle, inMessage) {
    Dialog.alert({ title: inTitle, message: inMessage });
  },

  // 响应登录正确接口...
  onLoginSuccess: function() {
    this.setData({ 
      m_show_auth: 2, 
      m_bgColor: '#eee',
      m_userInfo: g_app.globalData.m_userInfo
    });
  },

  /**
   * 生命周期函数--监听页面初次渲染完成
   */
  onReady: function () {

  },

  /**
   * 生命周期函数--监听页面显示
   */
  onShow: function () {

  },

  /**
   * 生命周期函数--监听页面隐藏
   */
  onHide: function () {

  },

  /**
   * 生命周期函数--监听页面卸载
   */
  onUnload: function () {

  },

  /**
   * 页面相关事件处理函数--监听用户下拉动作
   */
  onPullDownRefresh: function () {

  },

  /**
   * 页面上拉触底事件的处理函数
   */
  onReachBottom: function () {

  },

  /**
   * 用户点击右上角分享
   */
  onShareAppMessage: function () {

  }
})